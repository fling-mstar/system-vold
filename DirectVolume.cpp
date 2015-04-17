/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <linux/kdev_t.h>

#define LOG_TAG "DirectVolume"

#include <cutils/log.h>
#include <sysutils/NetlinkEvent.h>

#include "DirectVolume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "cryptfs.h"

// #define PARTITION_DEBUG

DirectVolume::DirectVolume(VolumeManager *vm, const fstab_rec* rec, int flags) :
        Volume(vm, rec, flags) {
    mPaths = new PathCollection();
    // MStar Android Patch Begin
    mPartMinors[0] = -1;
    // MStar Android Patch End
    mPendingPartMap = 0;
    mDiskMajor = -1;
    mDiskMinor = -1;
    mDiskNumParts = 0;

    if (strcmp(rec->mount_point, "auto") != 0) {
        ALOGE("Vold managed volumes must have auto mount point; ignoring %s",
              rec->mount_point);
    }

    // MStar Android Patch Begin
    mMountpoint = strdup(rec->mount_point);
    if (flags & VOL_PROVIDES_ASEC) {
        const char* externalStorage = getenv("EXTERNAL_STORAGE");
        if (externalStorage == NULL) {
            externalStorage = "/mnt/sdcard";
        }
        mFuseMountpoint = strdup(externalStorage);
    } else {
        mFuseMountpoint = strdup(rec->mount_point);
    }
    // MStar Android Patch End

    setState(Volume::State_NoMedia);
}

DirectVolume::~DirectVolume() {
    PathCollection::iterator it;

    for (it = mPaths->begin(); it != mPaths->end(); ++it)
        free(*it);
    delete mPaths;

    // MStar Android Patch Begin
    if (mMountpoint != NULL) {
        free((void*)mMountpoint);
    }

    if (mFuseMountpoint != NULL) {
        free((void*)mFuseMountpoint);
    }
    // MStar Android Patch End
}

int DirectVolume::addPath(const char *path) {
    mPaths->push_back(strdup(path));
    return 0;
}

dev_t DirectVolume::getDiskDevice() {
    return MKDEV(mDiskMajor, mDiskMinor);
}

dev_t DirectVolume::getShareDevice() {
    // MStar Android Patch Begin
    return MKDEV(mDiskMajor, mDiskMinor);
    // MStar Android Patch End
}

void DirectVolume::handleVolumeShared() {
    setState(Volume::State_Shared);
}

void DirectVolume::handleVolumeUnshared() {
    setState(Volume::State_Idle);
}

int DirectVolume::handleBlockEvent(NetlinkEvent *evt) {
    const char *dp = evt->findParam("DEVPATH");
    // MStar Android Patch Begin
    const char *dn = evt->findParam("DEVNAME");

    errno = ENODEV;
/*
    if (strcmp(dn, mLabel)) {
#ifdef PARTITION_DEBUG
        SLOGW("device name mismatch : (%s, %s %i)", dn, mLabel,getState());
#endif
        return -1;
    }
*/
    /* Only the volumes specificed in "vold.fstab" can have paths */
    if (mPaths->size()) {
        PathCollection::iterator  it;
        for (it = mPaths->begin(); it != mPaths->end(); ++it) {
            if (!strncmp(dp, *it, strlen(*it))) {
                break;
            }
        }
        if (it == mPaths->end()){
            return -1;
        }
    }

    /* We can handle this disk */
    int action = evt->getAction();
    const char *devtype = evt->findParam("DEVTYPE");

    if (!strcmp(devtype, "partition")) {
        const char *tmp = evt->findParam("PARTN");
        if (tmp) {
            if (atoi(tmp) != mPartIdx) {
            #ifdef PARTITION_DEBUG
                SLOGW("PartIdx mismatch");
            #endif
                return -1;
            }
        } else {
        #ifdef PARTITION_DEBUG
            SLOGW("Kernel block uevent missing 'PARTN'");
        #endif
            return -1;
        }
    }

    if (action == NetlinkEvent::NlActionAdd) {
        int major = atoi(evt->findParam("MAJOR"));
        int minor = atoi(evt->findParam("MINOR"));
        char nodepath[255];

        if (getState() != Volume::State_NoMedia ) {
            return 0;
        }

        mDiskMajor = atoi(evt->findParam("MAJOR"));
        mDiskMinor = atoi(evt->findParam("MINOR"));

        snprintf(nodepath,sizeof(nodepath), "/dev/block/vold/%d:%d",
                         major, minor);
        if (createDeviceNode(nodepath, major, minor)) {
            SLOGE("Error making device node '%s' (%s)", nodepath,strerror(errno));
        }

        if (!strcmp(devtype, "disk")) {
            handleDiskAdded(dp, evt);
        } else {
            handlePartitionAdded(dp, evt);
        }
    } else if (action == NetlinkEvent::NlActionRemove) {

        if (getState() == Volume::State_NoMedia ) {
            return 0;
        }

        if (!strcmp(devtype, "disk")) {
            handleDiskRemoved(dp, evt);
        } else {
            handlePartitionRemoved(dp, evt);
        }
    } else if (action == NetlinkEvent::NlActionChange) {
        if (!strcmp(devtype, "disk")) {
            handleDiskChanged(dp, evt);
        } else {
            handlePartitionChanged(dp, evt);
        }
    } else {
            SLOGW("Ignoring non add/remove/change event");
    }

    return 0;
    // MStar Android Patch End
}

void DirectVolume::handleDiskAdded(const char *devpath, NetlinkEvent *evt) {
    // MStar Android Patch Begin
    const char *tmp = evt->findParam("NPARTS");
    if (tmp) {
        mDiskNumParts = atoi(tmp);
    } else {
        SLOGW("Kernel block uevent missing 'NPARTS'");
        mDiskNumParts = 1;
    }

    char msg[255];

    int partmask = 0;
    int i;
    for (i = 1; i <= mDiskNumParts; i++) {
        partmask |= (1 << i);
    }
    mPendingPartMap = partmask;

    if (mDiskNumParts == 0) {
#ifdef PARTITION_DEBUG
        SLOGD("Dv::diskIns - No partitions - good to go son!");
#endif
        setState(Volume::State_Idle);
        snprintf(msg, sizeof(msg), "Volume %s %s disk inserted (%d:%d)",
             getLabel(), getFuseMountpoint(), mDiskMajor, mDiskMinor);
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskInserted,
                                             msg, false);
    } else {
#ifdef PARTITION_DEBUG
        SLOGD("Dv::diskIns - waiting for %d partitions (mask 0x%x)",
             mDiskNumParts, mPendingPartMap);
#endif
        setState(Volume::State_Pending);
    }
    // MStar Android Patch End
}

void DirectVolume::handlePartitionAdded(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    // MStar Android Patch Begin
    char msg[255];
    // MStar Android Patch End

    int part_num;

    const char *tmp = evt->findParam("PARTN");

    if (tmp) {
        part_num = atoi(tmp);
    } else {
        SLOGW("Kernel block uevent missing 'PARTN'");
        part_num = 1;
    }

    if (part_num > MAX_PARTITIONS || part_num < 1) {
        SLOGE("Invalid 'PARTN' value");
        return;
    }

    if (part_num > mDiskNumParts) {
        mDiskNumParts = part_num;
    }

    if (major != mDiskMajor) {
        SLOGE("Partition '%s' has a different major than its disk!", devpath);
        return;
    }
#ifdef PARTITION_DEBUG
    SLOGD("Dv:partAdd: part_num = %d, minor = %d\n", part_num, minor);
#endif
    // MStar Android Patch Begin
    mPartMinors[0] = minor;

    mPendingPartMap &= ~(1 << part_num);
    if (!mPendingPartMap) {
#ifdef PARTITION_DEBUG
        SLOGD("Dv:partAdd: Got all partitions - ready to rock!");
#endif
        if (getState() != Volume::State_Formatting) {
            setState(Volume::State_Idle);
            snprintf(msg, sizeof(msg), "Volume %s %s Partition Added (%d:%d)",
             getLabel(), getFuseMountpoint(), mDiskMajor, mDiskMinor);
            mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskInserted,
                                                msg, false);
        }
    } else {
#ifdef PARTITION_DEBUG
        SLOGD("Dv:partAdd: pending mask now = 0x%x", mPendingPartMap);
#endif
    }
    // MStar Android Patch End
}

void DirectVolume::handleDiskChanged(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));

    if ((major != mDiskMajor) || (minor != mDiskMinor)) {
        return;
    }

    SLOGI("Volume %s disk has changed", getLabel());
    const char *tmp = evt->findParam("NPARTS");
    if (tmp) {
        mDiskNumParts = atoi(tmp);
    } else {
        SLOGW("Kernel block uevent missing 'NPARTS'");
        mDiskNumParts = 1;
    }

    int partmask = 0;
    int i;
    for (i = 1; i <= mDiskNumParts; i++) {
        partmask |= (1 << i);
    }
    mPendingPartMap = partmask;

    if (getState() != Volume::State_Formatting) {
        if (mDiskNumParts == 0) {
            setState(Volume::State_Idle);
        } else {
            setState(Volume::State_Pending);
        }
    }
}

void DirectVolume::handlePartitionChanged(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    SLOGD("Volume %s %s partition %d:%d changed\n", getLabel(), getMountpoint(), major, minor);
}

void DirectVolume::handleDiskRemoved(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    char msg[255];

    // MStar Android Patch Begin
    SLOGD("Volume %s %s disk %d:%d removed\n", getLabel(), getMountpoint(), major, minor);
    if (getState() != Volume::State_Pending) {
        handlePartitionRemoved(devpath,evt);
    } else {
        setState(Volume::State_NoMedia);
    }
    // MStar Android Patch End
}

void DirectVolume::handlePartitionRemoved(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    char msg[255];
    int state;

    SLOGD("Volume %s %s partition %d:%d removed\n", getLabel(), getMountpoint(), major, minor);

    /*
     * The framework doesn't need to get notified of
     * partition removal unless it's mounted. Otherwise
     * the removal notification will be sent on the Disk
     * itself
     */
    state = getState();
    // MStar Android Patch Begin
    while (state == State_Checking) {
        usleep(500*1000);
        state = getState();
    }

    if (state != Volume::State_Mounted && state != Volume::State_Shared) {
        setState(Volume::State_NoMedia);
        snprintf(msg, sizeof(msg), "Volume %s %s bad removal (%d:%d)",
                getLabel(), getFuseMountpoint(), major, minor);
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskRemoved,
                                                 msg, false);
        SLOGE(msg);
        return;
    }
        
    if ((dev_t) MKDEV(major, minor) == mCurrentlyMountedKdev) {
        /*
         * Yikes, our mounted partition is going away!
         */
        if (Volume::unmountVol(true, false)) {
            SLOGE("Failed to unmount volume on bad removal (%s)",
                 strerror(errno));
            // XXX: At this point we're screwed for now
        } else {
            SLOGD("Crisis averted");
            setState(Volume::State_NoMedia);
        }
    } else if (state == Volume::State_Shared) {
        /* removed during mass storage */
        if (mVm->unshareVolume(getLabel(), "ums")) {
            SLOGE("Failed to unshare volume on bad removal (%s)",
                strerror(errno));
        } else {
            SLOGD("Crisis averted");
        }
        setState(Volume::State_NoMedia);
    }
    snprintf(msg, sizeof(msg), "Volume %s %s bad removal (%d:%d)",
                 getLabel(), getFuseMountpoint(), major, minor);
    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskRemoved,
                                             msg, false);
    // MStar Android Patch End
}

/*
 * Called from base to get a list of devicenodes for mounting
 */
int DirectVolume::getDeviceNodes(dev_t *devs, int max) {

    // MStar Android Patch Begin
    if (mPartIdx == -1) {
        // If the disk has no partitions, try the disk itself
        if (!mDiskNumParts) {
            devs[0] = MKDEV(mDiskMajor, mDiskMinor);
            return 1;
        }
    }
    devs[0] = MKDEV(mDiskMajor, mPartMinors[0]);
    // MStar Android Patch End
    return 1;
}

/*
 * Called from base to update device info,
 * e.g. When setting up an dm-crypt mapping for the sd card.
 */
int DirectVolume::updateDeviceInfo(char *new_path, int new_major, int new_minor)
{
    PathCollection::iterator it;

    if (mPartIdx == -1) {
        SLOGE("Can only change device info on a partition\n");
        return -1;
    }

    /*
     * This is to change the sysfs path associated with a partition, in particular,
     * for an internal SD card partition that is encrypted.  Thus, the list is
     * expected to be only 1 entry long.  Check that and bail if not.
     */
    if (mPaths->size() != 1) {
        SLOGE("Cannot change path if there are more than one for a volume\n");
        return -1;
    }

    it = mPaths->begin();
    free(*it); /* Free the string storage */
    mPaths->erase(it); /* Remove it from the list */
    addPath(new_path); /* Put the new path on the list */

    /* Save away original info so we can restore it when doing factory reset.
     * Then, when doing the format, it will format the original device in the
     * clear, otherwise it just formats the encrypted device which is not
     * readable when the device boots unencrypted after the reset.
     */
    mOrigDiskMajor = mDiskMajor;
    mOrigDiskMinor = mDiskMinor;
    mOrigPartIdx = mPartIdx;
    memcpy(mOrigPartMinors, mPartMinors, sizeof(mPartMinors));

    mDiskMajor = new_major;
    mDiskMinor = new_minor;
    /* Ugh, virual block devices don't use minor 0 for whole disk and minor > 0 for
     * partition number.  They don't have partitions, they are just virtual block
     * devices, and minor number 0 is the first dm-crypt device.  Luckily the first
     * dm-crypt device is for the userdata partition, which gets minor number 0, and
     * it is not managed by vold.  So the next device is minor number one, which we
     * will call partition one.
     */
    mPartIdx = new_minor;
    // MStar Android Patch Begin
    mPartMinors[0] = new_minor;
    // MStar Android Patch End

    mIsDecrypted = 1;

    return 0;
}

/*
 * Called from base to revert device info to the way it was before a
 * crypto mapping was created for it.
 */
void DirectVolume::revertDeviceInfo(void)
{
    if (mIsDecrypted) {
        mDiskMajor = mOrigDiskMajor;
        mDiskMinor = mOrigDiskMinor;
        mPartIdx = mOrigPartIdx;
        memcpy(mPartMinors, mOrigPartMinors, sizeof(mPartMinors));

        mIsDecrypted = 0;
    }

    return;
}

/*
 * Called from base to give cryptfs all the info it needs to encrypt eligible volumes
 */
int DirectVolume::getVolInfo(struct volume_info *v)
{
    strcpy(v->label, mLabel);
    strcpy(v->mnt_point, mMountpoint);
    v->flags = getFlags();
    /* Other fields of struct volume_info are filled in by the caller or cryptfs.c */

    return 0;
}
