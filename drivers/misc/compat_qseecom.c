// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compat ioctl translation layer for the qseecom driver.
 *
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * 32-bit callers reach the qseecom driver with the COMPAT_QSEECOM_IOCTL_*
 * command set and 32-bit sized request structures, while the driver only
 * implements the 64-bit QSEECOM_IOCTL_* set; this file translates between
 * the two.
 * compat_alloc_user_space() has been removed from this kernel, so the 4.9
 * scheme of staging a translated 64-bit request block in compat user memory
 * is replaced by per-field get_user() into a 64-bit request structure on
 * the kernel stack, a single qseecom_ioctl() call with that structure, and
 * per-field put_user() of the output fields back into the 32-bit buffer.
 * Pointer-sized fields are carried over numerically: the compat_uptr_t
 * value is zero-extended into the 64-bit pointer field without any address
 * translation; the native handlers only compare such values or convert
 * them into offsets and never dereference them as kernel pointers, which
 * is exactly the semantics the 4.9 compat layer relied upon.
 */

#if IS_ENABLED(CONFIG_COMPAT)

#include <linux/kernel.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/qseecom.h>
#include <asm/compat.h>
#include "compat_qseecom.h"

static int compat_get_qseecom_register_listener_req(
		struct compat_qseecom_register_listener_req __user *data32,
		struct qseecom_register_listener_req *data)
{
	compat_uptr_t p;
	int err;

	err  = get_user(data->listener_id, &data32->listener_id);
	err |= get_user(data->ifd_data_fd, &data32->ifd_data_fd);
	err |= get_user(p, &data32->virt_sb_base);
	data->virt_sb_base = (void *)(unsigned long)p;
	err |= get_user(data->sb_size, &data32->sb_size);

	return err;
}

static int compat_get_qseecom_load_img_req(
		struct compat_qseecom_load_img_req __user *data32,
		struct qseecom_load_img_req *data)
{
	int err;

	err  = get_user(data->mdt_len, &data32->mdt_len);
	err |= get_user(data->img_len, &data32->img_len);
	err |= get_user(data->ifd_data_fd, &data32->ifd_data_fd);
	if (copy_from_user(data->img_name, data32->img_name,
					sizeof(data->img_name)))
		return -EFAULT;
	err |= get_user(data->app_arch, &data32->app_arch);

	return err;
}

static int compat_put_qseecom_load_img_req(
		struct compat_qseecom_load_img_req __user *data32,
		struct qseecom_load_img_req *data)
{
	return put_user(data->app_id, &data32->app_id);
}

static int compat_get_qseecom_send_cmd_req(
		struct compat_qseecom_send_cmd_req __user *data32,
		struct qseecom_send_cmd_req *data)
{
	compat_uptr_t p;
	int err;

	err  = get_user(p, &data32->cmd_req_buf);
	data->cmd_req_buf = (void *)(unsigned long)p;
	err |= get_user(data->cmd_req_len, &data32->cmd_req_len);
	err |= get_user(p, &data32->resp_buf);
	data->resp_buf = (void *)(unsigned long)p;
	err |= get_user(data->resp_len, &data32->resp_len);

	return err;
}

static int compat_get_qseecom_send_modfd_cmd_req(
		struct compat_qseecom_send_modfd_cmd_req __user *data32,
		struct qseecom_send_modfd_cmd_req *data)
{
	compat_uptr_t p;
	int err;
	int i;

	err  = get_user(p, &data32->cmd_req_buf);
	data->cmd_req_buf = (void *)(unsigned long)p;
	err |= get_user(data->cmd_req_len, &data32->cmd_req_len);
	err |= get_user(p, &data32->resp_buf);
	data->resp_buf = (void *)(unsigned long)p;
	err |= get_user(data->resp_len, &data32->resp_len);

	for (i = 0; i < MAX_ION_FD; i++) {
		err |= get_user(data->ifd_data[i].fd,
					&data32->ifd_data[i].fd);
		err |= get_user(data->ifd_data[i].cmd_buf_offset,
					&data32->ifd_data[i].cmd_buf_offset);
	}

	return err;
}

static int compat_get_qseecom_set_sb_mem_param_req(
		struct compat_qseecom_set_sb_mem_param_req __user *data32,
		struct qseecom_set_sb_mem_param_req *data)
{
	compat_uptr_t p;
	int err;

	err  = get_user(data->ifd_data_fd, &data32->ifd_data_fd);
	err |= get_user(p, &data32->virt_sb_base);
	data->virt_sb_base = (void *)(unsigned long)p;
	err |= get_user(data->sb_len, &data32->sb_len);

	return err;
}

static int compat_get_qseecom_qseos_version_req(
		struct compat_qseecom_qseos_version_req __user *data32,
		struct qseecom_qseos_version_req *data)
{
	return get_user(data->qseos_version, &data32->qseos_version);
}

static int compat_put_qseecom_qseos_version_req(
		struct compat_qseecom_qseos_version_req __user *data32,
		struct qseecom_qseos_version_req *data)
{
	return put_user(data->qseos_version, &data32->qseos_version);
}

static int compat_get_int(compat_int_t __user *data32, int *data)
{
	return get_user(*data, data32);
}

static int compat_get_qseecom_qseos_app_load_query(
		struct compat_qseecom_qseos_app_load_query __user *data32,
		struct qseecom_qseos_app_load_query *data)
{
	int err;

	if (copy_from_user(data->app_name, data32->app_name,
					sizeof(data->app_name)))
		return -EFAULT;

	err = get_user(data->app_arch, &data32->app_arch);

	return err;
}

static int compat_put_qseecom_qseos_app_load_query(
		struct compat_qseecom_qseos_app_load_query __user *data32,
		struct qseecom_qseos_app_load_query *data)
{
	return put_user(data->app_id, &data32->app_id);
}

static int compat_get_qseecom_send_svc_cmd_req(
		struct compat_qseecom_send_svc_cmd_req __user *data32,
		struct qseecom_send_svc_cmd_req *data)
{
	compat_uptr_t p;
	int err;

	err  = get_user(data->cmd_id, &data32->cmd_id);
	err |= get_user(p, &data32->cmd_req_buf);
	data->cmd_req_buf = (void *)(unsigned long)p;
	err |= get_user(data->cmd_req_len, &data32->cmd_req_len);
	err |= get_user(p, &data32->resp_buf);
	data->resp_buf = (void *)(unsigned long)p;
	err |= get_user(data->resp_len, &data32->resp_len);

	return err;
}

static int compat_get_qseecom_create_key_req(
		struct compat_qseecom_create_key_req __user *data32,
		struct qseecom_create_key_req *data)
{
	if (copy_from_user(data->hash32, data32->hash32,
					sizeof(data->hash32)))
		return -EFAULT;

	return get_user(data->usage, &data32->usage);
}

static int compat_get_qseecom_wipe_key_req(
		struct compat_qseecom_wipe_key_req __user *data32,
		struct qseecom_wipe_key_req *data)
{
	int err;

	err  = get_user(data->usage, &data32->usage);
	err |= get_user(data->wipe_key_flag, &data32->wipe_key_flag);

	return err;
}

static int compat_get_qseecom_update_key_userinfo_req(
		struct compat_qseecom_update_key_userinfo_req __user *data32,
		struct qseecom_update_key_userinfo_req *data)
{
	if (copy_from_user(data->current_hash32, data32->current_hash32,
					sizeof(data->current_hash32)))
		return -EFAULT;
	if (copy_from_user(data->new_hash32, data32->new_hash32,
					sizeof(data->new_hash32)))
		return -EFAULT;

	return get_user(data->usage, &data32->usage);
}

static int compat_get_qseecom_save_partition_hash_req(
		struct compat_qseecom_save_partition_hash_req __user *data32,
		struct qseecom_save_partition_hash_req *data)
{
	int err;

	err  = get_user(data->partition_id, &data32->partition_id);
	if (copy_from_user(data->digest, data32->digest,
					sizeof(data->digest)))
		return -EFAULT;

	return err;
}

static int compat_get_qseecom_is_es_activated_req(
		struct compat_qseecom_is_es_activated_req __user *data32,
		struct qseecom_is_es_activated_req *data)
{
	return get_user(data->is_activated, &data32->is_activated);
}

static int compat_put_qseecom_is_es_activated_req(
		struct compat_qseecom_is_es_activated_req __user *data32,
		struct qseecom_is_es_activated_req *data)
{
	return put_user(data->is_activated, &data32->is_activated);
}

static int compat_get_qseecom_mdtp_cipher_dip_req(
		struct compat_qseecom_mdtp_cipher_dip_req __user *data32,
		struct qseecom_mdtp_cipher_dip_req *data)
{
	compat_uptr_t p;
	int err;

	err  = get_user(p, &data32->in_buf);
	data->in_buf = (uint8_t *)(unsigned long)p;
	err |= get_user(data->in_buf_size, &data32->in_buf_size);
	err |= get_user(p, &data32->out_buf);
	data->out_buf = (uint8_t *)(unsigned long)p;
	err |= get_user(data->out_buf_size, &data32->out_buf_size);
	err |= get_user(data->direction, &data32->direction);

	return err;
}

static int compat_get_qseecom_send_modfd_listener_resp(
		struct compat_qseecom_send_modfd_listener_resp __user *data32,
		struct qseecom_send_modfd_listener_resp *data)
{
	compat_uptr_t p;
	int err;
	int i;

	err  = get_user(p, &data32->resp_buf_ptr);
	data->resp_buf_ptr = (void *)(unsigned long)p;
	err |= get_user(data->resp_len, &data32->resp_len);

	for (i = 0; i < MAX_ION_FD; i++) {
		err |= get_user(data->ifd_data[i].fd,
					&data32->ifd_data[i].fd);
		err |= get_user(data->ifd_data[i].cmd_buf_offset,
					&data32->ifd_data[i].cmd_buf_offset);
	}

	return err;
}

static int compat_get_qseecom_qteec_req(
		struct compat_qseecom_qteec_req __user *data32,
		struct qseecom_qteec_req *data)
{
	compat_uptr_t p;
	int err;

	err  = get_user(p, &data32->req_ptr);
	data->req_ptr = (void *)(unsigned long)p;
	err |= get_user(data->req_len, &data32->req_len);
	err |= get_user(p, &data32->resp_ptr);
	data->resp_ptr = (void *)(unsigned long)p;
	err |= get_user(data->resp_len, &data32->resp_len);

	return err;
}

static int compat_get_qseecom_qteec_modfd_req(
		struct compat_qseecom_qteec_modfd_req __user *data32,
		struct qseecom_qteec_modfd_req *data)
{
	compat_uptr_t p;
	int err;
	int i;

	err  = get_user(p, &data32->req_ptr);
	data->req_ptr = (void *)(unsigned long)p;
	err |= get_user(data->req_len, &data32->req_len);
	err |= get_user(p, &data32->resp_ptr);
	data->resp_ptr = (void *)(unsigned long)p;
	err |= get_user(data->resp_len, &data32->resp_len);

	for (i = 0; i < MAX_ION_FD; i++) {
		err |= get_user(data->ifd_data[i].fd,
					&data32->ifd_data[i].fd);
		err |= get_user(data->ifd_data[i].cmd_buf_offset,
					&data32->ifd_data[i].cmd_buf_offset);
	}

	return err;
}

static unsigned int convert_cmd(unsigned int cmd)
{
	switch (cmd) {
	case COMPAT_QSEECOM_IOCTL_REGISTER_LISTENER_REQ:
		return QSEECOM_IOCTL_REGISTER_LISTENER_REQ;
	case COMPAT_QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
		return QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_CMD_REQ:
		return QSEECOM_IOCTL_SEND_CMD_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
		return QSEECOM_IOCTL_SEND_MODFD_CMD_REQ;
	case COMPAT_QSEECOM_IOCTL_RECEIVE_REQ:
		return QSEECOM_IOCTL_RECEIVE_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_RESP_REQ:
		return QSEECOM_IOCTL_SEND_RESP_REQ;
	case COMPAT_QSEECOM_IOCTL_LOAD_APP_REQ:
		return QSEECOM_IOCTL_LOAD_APP_REQ;
	case COMPAT_QSEECOM_IOCTL_SET_MEM_PARAM_REQ:
		return QSEECOM_IOCTL_SET_MEM_PARAM_REQ;
	case COMPAT_QSEECOM_IOCTL_UNLOAD_APP_REQ:
		return QSEECOM_IOCTL_UNLOAD_APP_REQ;
	case COMPAT_QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ:
		return QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ;
	case COMPAT_QSEECOM_IOCTL_PERF_ENABLE_REQ:
		return QSEECOM_IOCTL_PERF_ENABLE_REQ;
	case COMPAT_QSEECOM_IOCTL_PERF_DISABLE_REQ:
		return QSEECOM_IOCTL_PERF_DISABLE_REQ;
	case COMPAT_QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ:
		return QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ;
	case COMPAT_QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ:
		return QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ;
	case COMPAT_QSEECOM_IOCTL_APP_LOADED_QUERY_REQ:
		return QSEECOM_IOCTL_APP_LOADED_QUERY_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_CMD_SERVICE_REQ:
		return QSEECOM_IOCTL_SEND_CMD_SERVICE_REQ;
	case COMPAT_QSEECOM_IOCTL_CREATE_KEY_REQ:
		return QSEECOM_IOCTL_CREATE_KEY_REQ;
	case COMPAT_QSEECOM_IOCTL_WIPE_KEY_REQ:
		return QSEECOM_IOCTL_WIPE_KEY_REQ;
	case COMPAT_QSEECOM_IOCTL_SAVE_PARTITION_HASH_REQ:
		return QSEECOM_IOCTL_SAVE_PARTITION_HASH_REQ;
	case COMPAT_QSEECOM_IOCTL_IS_ES_ACTIVATED_REQ:
		return QSEECOM_IOCTL_IS_ES_ACTIVATED_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_RESP:
		return QSEECOM_IOCTL_SEND_MODFD_RESP;
	case COMPAT_QSEECOM_IOCTL_SET_BUS_SCALING_REQ:
		return QSEECOM_IOCTL_SET_BUS_SCALING_REQ;
	case COMPAT_QSEECOM_IOCTL_UPDATE_KEY_USER_INFO_REQ:
		return QSEECOM_IOCTL_UPDATE_KEY_USER_INFO_REQ;
	case COMPAT_QSEECOM_QTEEC_IOCTL_OPEN_SESSION_REQ:
		return QSEECOM_QTEEC_IOCTL_OPEN_SESSION_REQ;
	case COMPAT_QSEECOM_QTEEC_IOCTL_CLOSE_SESSION_REQ:
		return QSEECOM_QTEEC_IOCTL_CLOSE_SESSION_REQ;
	case COMPAT_QSEECOM_QTEEC_IOCTL_INVOKE_MODFD_CMD_REQ:
		return QSEECOM_QTEEC_IOCTL_INVOKE_MODFD_CMD_REQ;
	case COMPAT_QSEECOM_QTEEC_IOCTL_REQUEST_CANCELLATION_REQ:
		return QSEECOM_QTEEC_IOCTL_REQUEST_CANCELLATION_REQ;
	case COMPAT_QSEECOM_IOCTL_MDTP_CIPHER_DIP_REQ:
		return QSEECOM_IOCTL_MDTP_CIPHER_DIP_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ:
		return QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ;
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_RESP_64:
		return QSEECOM_IOCTL_SEND_MODFD_RESP_64;
	default:
		break;
	}

	return cmd;
}

long compat_qseecom_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	long ret;

	switch (cmd) {

	case COMPAT_QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
	case COMPAT_QSEECOM_IOCTL_RECEIVE_REQ:
	case COMPAT_QSEECOM_IOCTL_SEND_RESP_REQ:
	case COMPAT_QSEECOM_IOCTL_UNLOAD_APP_REQ:
	case COMPAT_QSEECOM_IOCTL_PERF_ENABLE_REQ:
	case COMPAT_QSEECOM_IOCTL_PERF_DISABLE_REQ:
	case COMPAT_QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ: {
		return qseecom_do_ioctl(file, convert_cmd(cmd), NULL);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_REGISTER_LISTENER_REQ: {
		struct compat_qseecom_register_listener_req __user *data32;
		struct qseecom_register_listener_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_register_listener_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_LOAD_APP_REQ: {
		struct compat_qseecom_load_img_req __user *data32;
		struct qseecom_load_img_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_load_img_req(data32, &data);
		if (err)
			return err;

		ret = qseecom_do_ioctl(file, convert_cmd(cmd), &data);
		err = compat_put_qseecom_load_img_req(data32, &data);
		return ret ? ret : err;
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SEND_CMD_REQ: {
		struct compat_qseecom_send_cmd_req __user *data32;
		struct qseecom_send_cmd_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_send_cmd_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ: {
		struct compat_qseecom_send_modfd_cmd_req __user *data32;
		struct qseecom_send_modfd_cmd_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_send_modfd_cmd_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SET_MEM_PARAM_REQ: {
		struct compat_qseecom_set_sb_mem_param_req __user *data32;
		struct qseecom_set_sb_mem_param_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_set_sb_mem_param_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ: {
		struct compat_qseecom_qseos_version_req __user *data32;
		struct qseecom_qseos_version_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_qseos_version_req(data32, &data);
		if (err)
			return err;

		ret = qseecom_do_ioctl(file, convert_cmd(cmd), &data);
		err = compat_put_qseecom_qseos_version_req(data32, &data);

		return ret ? ret : err;
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SET_BUS_SCALING_REQ: {
		compat_int_t __user *data32;
		int data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_int(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ: {
		struct compat_qseecom_load_img_req __user *data32;
		struct qseecom_load_img_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_load_img_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_APP_LOADED_QUERY_REQ: {
		struct compat_qseecom_qseos_app_load_query __user *data32;
		struct qseecom_qseos_app_load_query data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_qseos_app_load_query(data32, &data);
		if (err)
			return err;

		ret = qseecom_do_ioctl(file, convert_cmd(cmd), &data);
		err = compat_put_qseecom_qseos_app_load_query(data32, &data);
		return ret ? ret : err;
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SEND_CMD_SERVICE_REQ: {
		struct compat_qseecom_send_svc_cmd_req __user *data32;
		struct qseecom_send_svc_cmd_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_send_svc_cmd_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_CREATE_KEY_REQ: {
		struct compat_qseecom_create_key_req __user *data32;
		struct qseecom_create_key_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_create_key_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_WIPE_KEY_REQ: {
		struct compat_qseecom_wipe_key_req __user *data32;
		struct qseecom_wipe_key_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_wipe_key_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_UPDATE_KEY_USER_INFO_REQ: {
		struct compat_qseecom_update_key_userinfo_req __user *data32;
		struct qseecom_update_key_userinfo_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_update_key_userinfo_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SAVE_PARTITION_HASH_REQ: {
		struct compat_qseecom_save_partition_hash_req __user *data32;
		struct qseecom_save_partition_hash_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_save_partition_hash_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_IS_ES_ACTIVATED_REQ: {
		struct compat_qseecom_is_es_activated_req __user *data32;
		struct qseecom_is_es_activated_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_is_es_activated_req(data32, &data);
		if (err)
			return err;

		ret = qseecom_do_ioctl(file, convert_cmd(cmd), &data);
		err = compat_put_qseecom_is_es_activated_req(data32, &data);
		return ret ? ret : err;
	}
	break;
	case COMPAT_QSEECOM_IOCTL_MDTP_CIPHER_DIP_REQ: {
		struct compat_qseecom_mdtp_cipher_dip_req __user *data32;
		struct qseecom_mdtp_cipher_dip_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_mdtp_cipher_dip_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_RESP:
	case COMPAT_QSEECOM_IOCTL_SEND_MODFD_RESP_64: {
		struct compat_qseecom_send_modfd_listener_resp __user *data32;
		struct qseecom_send_modfd_listener_resp data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_send_modfd_listener_resp(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_QTEEC_IOCTL_CLOSE_SESSION_REQ: {
		struct compat_qseecom_qteec_req __user *data32;
		struct qseecom_qteec_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_qteec_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	case COMPAT_QSEECOM_QTEEC_IOCTL_OPEN_SESSION_REQ:
	case COMPAT_QSEECOM_QTEEC_IOCTL_INVOKE_MODFD_CMD_REQ:
	case COMPAT_QSEECOM_QTEEC_IOCTL_REQUEST_CANCELLATION_REQ: {
		struct compat_qseecom_qteec_modfd_req __user *data32;
		struct qseecom_qteec_modfd_req data;
		int err;

		data32 = compat_ptr(arg);

		err = compat_get_qseecom_qteec_modfd_req(data32, &data);
		if (err)
			return err;

		return qseecom_do_ioctl(file, convert_cmd(cmd), &data);
	}
	break;
	default:
		return -ENOIOCTLCMD;
	break;
	}
	return 0;
}

#endif /* CONFIG_COMPAT */
