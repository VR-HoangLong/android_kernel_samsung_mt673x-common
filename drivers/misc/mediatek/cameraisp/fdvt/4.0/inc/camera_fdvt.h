/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __CAMERA_FDVT_H__
#define __CAMERA_FDVT_H__

#include <linux/ioctl.h>
#define FDVT_IOC_MAGIC    'N'

#define SIG_ERESTARTSYS 512

#ifdef CONFIG_COMPAT
/*64 bit*/
#include <linux/fs.h>
#include <linux/compat.h>
#endif

struct FDVTRegIO {
	unsigned int  *pAddr;
	unsigned int  *pData;
	unsigned int  u4Count;
};
#define FDVTRegIO struct FDVTRegIO

#ifdef CONFIG_COMPAT

struct compat_FDVTRegIO {
	compat_uptr_t pAddr;
	compat_uptr_t pData;
	unsigned int  u4Count;
};
#define compat_FDVTRegIO struct compat_FDVTRegIO

#endif

struct FDVTSecureMeta {
	unsigned int Learning_Type;
	unsigned int fd_mode;
	unsigned int source_img_width[15];
	unsigned int source_img_height[15];
	unsigned int RIP_feature;
	unsigned int GFD_skip;
	unsigned int GFD_skip_V;
	unsigned int feature_threshold;
	unsigned int source_img_fmt;
	bool scale_from_original;
	bool scale_manual_mode;
	unsigned int scale_num_from_user;
	bool dynamic_change_model[18];

	unsigned int ImgSrcY_Handler;
	unsigned int ImgSrcUV_Handler;
	unsigned int RSConfig_Handler;
	unsigned int RSOutBuf_Handler;
	unsigned int FDConfig_Handler;
	uint64_t     FDResultBuf_PA;
	unsigned int Learning_Data_Handler[18];
	unsigned int Extra_Learning_Data_Handler[18];

	unsigned int ImgSrc_Y_Size;
	unsigned int ImgSrc_UV_Size;
	unsigned int RSConfigSize;
	unsigned int RSOutBufSize;
	unsigned int FDConfigSize;
	unsigned int FDResultBufSize;
	unsigned int Learning_Data_Size[18];

	unsigned short SecMemType;
	bool CarvedOutResult;
	bool isReleased;
};
#define FDVTSecureMeta struct FDVTSecureMeta

struct FDVTMetaData {
	FDVTSecureMeta *SecureMeta;
};
#define FDVTMetaData struct FDVTMetaData

struct compat_FDVTMetaData {
	compat_uptr_t SecureMeta;
};
#define compat_FDVTMetaData struct compat_FDVTMetaData

/*below is control message*/
#define FDVT_IOC_INIT_SETPARA_CMD \
	_IO(FDVT_IOC_MAGIC, 0x00)
#define FDVT_IOC_STARTFD_CMD \
	_IO(FDVT_IOC_MAGIC, 0x01)
#define FDVT_IOC_G_WAITIRQ \
	_IOR(FDVT_IOC_MAGIC, 0x02, unsigned int)
#define FDVT_IOC_T_SET_FDCONF_CMD \
	_IOW(FDVT_IOC_MAGIC, 0x03, FDVTRegIO)
#define FDVT_IOC_G_READ_FDREG_CMD \
	_IOWR(FDVT_IOC_MAGIC, 0x04, FDVTRegIO)
#define FDVT_IOC_T_SET_SDCONF_CMD \
	_IOW(FDVT_IOC_MAGIC, 0x05, FDVTRegIO)
#define FDVT_IOC_INIT_SETNORMAL_CMD \
	_IO(FDVT_IOC_MAGIC, 0x06)
#define FDVT_IOC_INIT_SETSECURE_CMD \
	_IO(FDVT_IOC_MAGIC, 0x07)
#define FDVT_IOC_SETMETA_CMD \
	_IOW(FDVT_IOC_MAGIC, 0x08, FDVTMetaData)
#define FDVT_IOC_T_DUMPREG \
	_IO(FDVT_IOC_MAGIC, 0x80)

#ifdef CONFIG_COMPAT
#define COMPAT_FDVT_IOC_INIT_SETPARA_CMD \
	_IO(FDVT_IOC_MAGIC, 0x00)
#define COMPAT_FDVT_IOC_STARTFD_CMD \
	_IO(FDVT_IOC_MAGIC, 0x01)
#define COMPAT_FDVT_IOC_G_WAITIRQ \
	_IOR(FDVT_IOC_MAGIC, 0x02, unsigned int)
#define COMPAT_FDVT_IOC_T_SET_FDCONF_CMD \
	_IOW(FDVT_IOC_MAGIC, 0x03, compat_FDVTRegIO)
#define COMPAT_FDVT_IOC_G_READ_FDREG_CMD \
	_IOWR(FDVT_IOC_MAGIC, 0x04, compat_FDVTRegIO)
#define COMPAT_FDVT_IOC_T_SET_SDCONF_CMD \
	_IOW(FDVT_IOC_MAGIC, 0x05, compat_FDVTRegIO)
#define COMPAT_FDVT_IOC_INIT_SETNORMAL_CMD \
	_IO(FDVT_IOC_MAGIC, 0x06)
#define COMPAT_FDVT_IOC_INIT_SETSECURE_CMD \
	_IO(FDVT_IOC_MAGIC, 0x07)
#define COMPAT_FDVT_IOC_SETMETA_CMD \
	_IOW(FDVT_IOC_MAGIC, 0x08, compat_FDVTMetaData)
#define COMPAT_FDVT_IOC_T_DUMPREG \
	_IO(FDVT_IOC_MAGIC, 0x80)
#endif


#endif/*__CAMERA_FDVT_H__*/


























