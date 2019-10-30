/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2015 Intel Corporation
 * Copyright (C) 2018-2019 Eltan B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <mboot.h>
#include <assert.h>
#include <build.h>
#include <vb2_api.h>
#include <board_mboot.h>

/*
 * Get the list of currently active PCR banks in TPM.
 *
 * @retval A map of active PCR banks.
 */
EFI_TCG2_EVENT_ALGORITHM_BITMAP tpm2_get_active_pcrs(void)
{
	int status;
	TPML_PCR_SELECTION Pcrs;
	EFI_TCG2_EVENT_ALGORITHM_BITMAP tpmHashAlgorithmBitmap = 0;
	uint32_t activePcrBanks = 0;
	uint32_t index;

	status = tpm2_get_capability_pcrs(&Pcrs);
	if (status != TPM_SUCCESS) {
		tpmHashAlgorithmBitmap = EFI_TCG2_BOOT_HASH_ALG_SHA1;
		activePcrBanks = EFI_TCG2_BOOT_HASH_ALG_SHA1;
	} else {
		for (index = 0; index < Pcrs.count; index++) {
			switch (Pcrs.pcrSelections[index].hash) {
			case TPM_ALG_SHA1:
				tpmHashAlgorithmBitmap |=
					EFI_TCG2_BOOT_HASH_ALG_SHA1;
				if (!is_zero_buffer(Pcrs.pcrSelections[index].pcrSelect,
				  Pcrs.pcrSelections[index].sizeofSelect))
					activePcrBanks |=
						 EFI_TCG2_BOOT_HASH_ALG_SHA1;
				break;
			case TPM_ALG_SHA256:
				tpmHashAlgorithmBitmap |=
					EFI_TCG2_BOOT_HASH_ALG_SHA256;
				if (!is_zero_buffer(Pcrs.pcrSelections[index].pcrSelect,
				  Pcrs.pcrSelections[index].sizeofSelect))
					activePcrBanks |=
						EFI_TCG2_BOOT_HASH_ALG_SHA256;
				break;
			case TPM_ALG_SHA384:
			case TPM_ALG_SHA512:
			case TPM_ALG_SM3_256:
			default:
				printk(BIOS_DEBUG, "%s: unsupported algorithm "
					"reported - 0x%x\n", __func__,
					Pcrs.pcrSelections[index].hash);
				break;
			}
		}
	}
	printk(BIOS_DEBUG, "Tcg2 Capability values from TPM\n");
	printk(BIOS_DEBUG, "tpmHashAlgorithmBitmap - 0x%08x\n",
		tpmHashAlgorithmBitmap);
	printk(BIOS_DEBUG, "activePcrBanks         - 0x%08x\n",
		activePcrBanks);

	return activePcrBanks;
}

/*
 * tpm2_get_capability_pcrs
 *
 * Return the TPM PCR information.
 *
 * This function parses the data got from tlcl_get_capability and returns the
 * PcrSelection.
 *
 * @param[out] Pcrs		The Pcr Selection
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	The command was unsuccessful.
 */
int tpm2_get_capability_pcrs(TPML_PCR_SELECTION *Pcrs)
{
	TPMS_CAPABILITY_DATA TpmCap;
	int status;
	int index;

	status = tlcl_get_capability(TPM_CAP_PCRS, 0, 1, &TpmCap);
	if (status == TPM_SUCCESS) {
		Pcrs->count = TpmCap.data.assignedPCR.count;
		printk(BIOS_DEBUG, "Pcrs->count = %d\n", Pcrs->count);
		for (index = 0; index < Pcrs->count; index++) {
			Pcrs->pcrSelections[index].hash =
				swab16(TpmCap.data.assignedPCR.pcrSelections[index].hash);
			printk(BIOS_DEBUG, "Pcrs->pcrSelections[index].hash ="
				"0x%x\n", Pcrs->pcrSelections[index].hash);
			Pcrs->pcrSelections[index].sizeofSelect =
				TpmCap.data.assignedPCR.pcrSelections[index].sizeofSelect;
			memcpy(Pcrs->pcrSelections[index].pcrSelect,
				TpmCap.data.assignedPCR.pcrSelections[index].pcrSelect,
				Pcrs->pcrSelections[index].sizeofSelect);
		}
	}
	return status;
}

/*
 * mboot_hash_extend_log
 *
 * Calculates the hash over the data and extends it in active PCR banks and
 * then logs them in the event log.
 *
 * @param[in] activePcr		bitmap of active PCR banks in TPM.
 * @param[in] flags		flags associated with hash data. Currently
 *				unused.
 * @param[in] hashData		data to be hashed.
 * @param[in] hashDataLen	length of the data to be hashed.
 * @param[in] newEventHdr	event header in TCG_PCR_EVENT2 format.
 * @param[in] eventLog		description of the event.
 * @param[in] invalid		invalidate the pcr
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	Unexpected device behavior.
 */
int mboot_hash_extend_log(EFI_TCG2_EVENT_ALGORITHM_BITMAP activePcr,
	uint64_t flags, uint8_t *hashData, uint32_t hashDataLen,
	TCG_PCR_EVENT2_HDR *newEventHdr, uint8_t *eventLog, uint8_t invalid)
{
	int status;
	TPMT_HA *digest = NULL;
	int digest_num = 0;

	printk(BIOS_DEBUG, "%s: Hash Data Length: %zu bytes\n", __func__,
		(size_t)hashDataLen);

	if (invalid) {
		digest = &(newEventHdr->digest.digests[digest_num]);
		digest->hashAlg = TPM_ALG_ERROR;
		digest_num++;
	} else {
		/*
		 * Generate SHA1 hash if SHA1 PCR bank is active in TPM
		 * currently
		 */
		if (activePcr & EFI_TCG2_BOOT_HASH_ALG_SHA1) {
			digest = &(newEventHdr->digest.digests[digest_num]);
			if (flags & MBOOT_HASH_PROVIDED) {
				/* The hash is provided as data */
				memcpy(digest->digest.sha1, (void *)hashData,
					VB2_SHA1_DIGEST_SIZE);
			} else {
				if (cb_sha_little_endian(VB2_HASH_SHA1, hashData,
						       hashDataLen, digest->digest.sha1))
					return TPM_E_IOERROR;
			}

			digest->hashAlg = TPM_ALG_SHA1;
			digest_num++;

			printk(BIOS_DEBUG, "%s: SHA1 Hash Digest:\n", __func__);
			mboot_print_buffer(digest->digest.sha1,
				VB2_SHA1_DIGEST_SIZE);
		}

		/*
		 * Generate SHA256 hash if SHA256 PCR bank is active in TPM
		 * currently
		 */
		if (activePcr & EFI_TCG2_BOOT_HASH_ALG_SHA256) {
			digest = &(newEventHdr->digest.digests[digest_num]);
			if (flags & MBOOT_HASH_PROVIDED) {
				/* The hash is provided as data */
				memcpy(digest->digest.sha256,
					(void *)hashData, hashDataLen);
			} else {

				if (cb_sha_little_endian(VB2_HASH_SHA256, hashData,
						       hashDataLen, digest->digest.sha256))
					return TPM_E_IOERROR;
			}
			digest->hashAlg = TPM_ALG_SHA256;
			digest_num++;

			printk(BIOS_DEBUG, "%s: SHA256 Hash Digest:\n",
				__func__);
			mboot_print_buffer(digest->digest.sha256,
				VB2_SHA256_DIGEST_SIZE);
		}
	}

	newEventHdr->digest.count = digest_num;

	status = tlcl_extend(newEventHdr->pcrIndex, (uint8_t *)&(newEventHdr->digest),
			NULL);
	if (status != TPM_SUCCESS)
		printk(BIOS_DEBUG, "%s: returned 0x%x\n", __func__, status);

	return status;
}

/*
 * invalidate_pcrs
 *
 * Invalidate PCRs 0-7 with extending 1 after tpm failure.
 */
void invalidate_pcrs(void)
{
	int status, pcr;
	TCG_PCR_EVENT2_HDR tcgEventHdr;
	EFI_TCG2_EVENT_ALGORITHM_BITMAP ActivePcrs;
	uint8_t invalidate;

	ActivePcrs = tpm2_get_active_pcrs();
	invalidate = 1;

	for (pcr = 0; pcr < 8; pcr++) {
		printk(BIOS_DEBUG, "%s: Invalidating PCR %d\n", __func__, pcr);
		memset(&tcgEventHdr, 0, sizeof(tcgEventHdr));
		tcgEventHdr.pcrIndex  = pcr;
		tcgEventHdr.eventType = EV_NO_ACTION;
		tcgEventHdr.eventSize = (uint32_t) sizeof(invalidate);

		status = mboot_hash_extend_log(ActivePcrs, 0,
			(uint8_t *)&invalidate, tcgEventHdr.eventSize,
			&tcgEventHdr, (uint8_t *)"Invalidate PCR", invalidate);

		if (status != TPM_SUCCESS)
			printk(BIOS_DEBUG, "%s: invalidating pcr %d returned"
				" 0x%x\n", __func__, pcr, status);
	}
}

/*
 * is_zero_buffer
 *
 * Check if buffer is all zero.
 *
 * @param[in] buffer   Buffer to be checked.
 * @param[in] size     Size of buffer to be checked.
 *
 * @retval TRUE  buffer is all zero.
 * @retval FALSE buffer is not all zero.
 */
int is_zero_buffer(void *buffer, unsigned int size)
{
	uint8_t *ptr;

	ptr = buffer;
	while (size--) {
		if (*(ptr++) != 0)
			return false;
	}
	return true;
}

/*
 * Prints command or response buffer for debugging purposes.
 *
 * @param[in] Buffer     Buffer to print.
 * @param[in] BufferSize Buffer data length.
 *
 * @retval  None
 */
void mboot_print_buffer(uint8_t *buffer, uint32_t bufferSize)
{
	uint32_t index;

	printk(BIOS_DEBUG, "Buffer Address: 0x%08x, Size: 0x%08x, Value:\n",
		(unsigned int)*buffer, bufferSize);
	for (index = 0; index < bufferSize; index++) {
		printk(BIOS_DEBUG, "%02x ", *(buffer + index));
		if ((index+1) % 16 == 0)
			printk(BIOS_DEBUG, "\n");
	}
	printk(BIOS_DEBUG, "\n");
}

/*
 * measures and logs the specified cbfs file.
 *
 * @param[in] activePcr		bitmap of active PCR banks in TPM.
 * @param[in] name		name of the cbfs file to measure
 * @param[in] type		data type of the cbfs file.
 * @param[in] pcr		pcr to extend.
 * @param[in] evenType		tcg event type.
 * @param[in] event_msg		description of the event.
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	Unexpected device behavior.
 */
int mb_measure_log_worker(EFI_TCG2_EVENT_ALGORITHM_BITMAP activePcr,
		const char *name, uint32_t type, uint32_t pcr,
		TCG_EVENTTYPE eventType, const char *event_msg)
{
	int status;
	TCG_PCR_EVENT2_HDR tcgEventHdr;
	uint8_t *base;
	size_t size;

	printk(BIOS_DEBUG, "%s: Measure %s\n", __func__, name);
	base = cbfs_boot_map_with_leak(name, type, &size);

	if (base == NULL) {
		printk(BIOS_DEBUG, "%s: CBFS locate fail: %s\n", __func__,
			name);
		return VB2_ERROR_READ_FILE_OPEN;
	}

	printk(BIOS_DEBUG, "%s: CBFS locate success: %s\n",
			__func__, name);
	memset(&tcgEventHdr, 0, sizeof(tcgEventHdr));
	tcgEventHdr.pcrIndex  = pcr;
	tcgEventHdr.eventType = eventType;
	if (event_msg)
		tcgEventHdr.eventSize = (uint32_t) strlen(event_msg);

	status = mboot_hash_extend_log(activePcr, 0, base, size, &tcgEventHdr,
		(uint8_t *)event_msg, 0);
	return status;
}

#ifdef __PRE_RAM__
/*
 * Called from early romstage
 *
 *mb_entry
 *
 * initializes measured boot mechanism, initializes the tpm library and starts the tpm called
 * by mb_measure
 *
 * The function can be overridden at the mainboard level my simply creating a function with the
 * same name there.
 *
 * @param[in] wake_from_s3	1 if we are waking from S3, 0 standard boot
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	Unexpected device behavior.
**/

int __attribute__((weak)) mb_entry(int wake_from_s3)
{
	int status;

	/* Initialize TPM driver. */
	printk(BIOS_DEBUG, "%s: tlcl_lib_init\n", __func__);
	if (tlcl_lib_init() != VB2_SUCCESS) {
		printk(BIOS_ERR, "%s: TPM driver initialization failed.\n", __func__);
		return TPM_E_IOERROR;
	}

	if (wake_from_s3) {
		printk(BIOS_DEBUG, "%s: tlcl_resume\n", __func__);
		status = tlcl_resume();
	} else {
		printk(BIOS_DEBUG, "%s: tlcl_startup\n", __func__);
		status = tlcl_startup();
	}

	if (status)
		printk(BIOS_ERR, "%s: StartUp failed 0x%x!\n", __func__, status);

	return status;
}

/*
 *
 * mb_measure
 *
 * initial call to the measured boot mechanism, initializes the
 * tpm library, starts the tpm and performs the measurements defined by
 * the coreboot platform.
 *
 * The pcrs will be invalidated if the measurement fails
 *
 * The function can be overridden at the mainboard level my simply creating a
 * function with the same name there.
 *
 * @param[in] wake_from_s3	1 if we are waking from S3, 0 standard boot
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	Unexpected device behavior.
 */

int __attribute__((weak))mb_measure(int wake_from_s3)
{
	uint32_t status;

	status = mb_entry(wake_from_s3);
	if (status == TPM_SUCCESS) {
		printk(BIOS_DEBUG, "%s: StartUp, successful!\n", __func__);
		status = mb_measure_log_start();
		if (status == TPM_SUCCESS) {
			printk(BIOS_DEBUG, "%s: Measuring, successful!\n", __func__);
		} else {
			invalidate_pcrs();
			printk(BIOS_ERR, "%s: Measuring returned 0x%x unsuccessful! PCRs invalidated.\n",
			       __func__, status);
		}
	} else {
		invalidate_pcrs();
		printk(BIOS_ERR, "%s: StartUp returned 0x%x, unsuccessful! PCRs invalidated.\n", __func__,
		       status);
	}
	return status;
}

/*
 *
 * mb_measure_log_start
 *
 * performs the measurements defined by the the board routines.
 *
 * The logging is defined by the mb_log_list structure
 *
 * These items need to be defined in the mainboard part of the mboot
 * implementation
 *
 * The function can be overridden at the mainboard level my simply creating a
 * function with the same name there.
 *
 * @param[in]  none
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	Unexpected device behavior.
 */
int __attribute__((weak))mb_measure_log_start(void)
{
	int status;
	EFI_TCG2_EVENT_ALGORITHM_BITMAP ActivePcrs;
	uint32_t i;

	ActivePcrs = tpm2_get_active_pcrs();

	if (ActivePcrs == 0x0) {
		printk(BIOS_DEBUG, "%s: No Active PCR Bank in TPM.\n",
			__func__);
		return TPM_E_IOERROR;
	}

	status = mb_crtm(ActivePcrs);
	if (status != TPM_SUCCESS) {
		printk(BIOS_DEBUG, "%s: Fail! CRTM Version can't be measured."
			" ABORTING!!!\n", __func__);
		return status;
	}
	printk(BIOS_DEBUG, "%s: Success! CRTM Version measured.\n", __func__);

	/* Log the items defined by the mainboard */
	for (i = 0; i < ARRAY_SIZE(mb_log_list); i++) {
		status = mb_measure_log_worker(
				ActivePcrs, mb_log_list[i].cbfs_name,
				mb_log_list[i].cbfs_type, mb_log_list[i].pcr,
				mb_log_list[i].eventType,
				mb_log_list[i].event_msg);
		if (status != TPM_SUCCESS) {
			printk(BIOS_DEBUG, "%s: Fail! %s can't be measured."
				"ABORTING!!!\n", __func__,
				mb_log_list[i].cbfs_name);
			return status;
		}
		printk(BIOS_DEBUG, "%s: Success! %s measured to pcr"
			"%d.\n", __func__, mb_log_list[i].cbfs_name,
			mb_log_list[i].pcr);
	}
	return status;
}

static const uint8_t crtm_version[] =
	CONFIG_VENDORCODE_ELTAN_CRTM_VERSION_STRING\
	COREBOOT_VERSION COREBOOT_EXTRA_VERSION " " COREBOOT_BUILD;

/*
 *
 * mb_crtm
 *
 * measures the crtm version. this consists of a string than can be
 * defined using make menuconfig and automatically generated version
 * information.
 *
 * The function can be overridden at the mainboard level my simply creating a
 * function with the same name there.
 *
 * @param[in] activePcr		bitmap of the support
 *
 * @retval TPM_SUCCESS		Operation completed successfully.
 * @retval TPM_E_IOERROR	Unexpected device behavior.
**/
int __attribute__((weak))mb_crtm(EFI_TCG2_EVENT_ALGORITHM_BITMAP activePcr)
{
	int status;
	TCG_PCR_EVENT2_HDR tcgEventHdr;
	uint8_t hash[VB2_SHA256_DIGEST_SIZE];
	uint8_t *msgPtr;

	/* Use FirmwareVersion string to represent CRTM version. */
	printk(BIOS_DEBUG, "%s: Measure CRTM Version\n", __func__);
	memset(&tcgEventHdr, 0, sizeof(tcgEventHdr));
	tcgEventHdr.pcrIndex = MBOOT_PCR_INDEX_0;
	tcgEventHdr.eventType = EV_S_CRTM_VERSION;
	tcgEventHdr.eventSize = sizeof(crtm_version);
	printk(BIOS_DEBUG, "%s: EventSize - %u\n", __func__,
		tcgEventHdr.eventSize);

	status = mboot_hash_extend_log(activePcr, 0, (uint8_t *)crtm_version,
		tcgEventHdr.eventSize, &tcgEventHdr, (uint8_t *)crtm_version,
		0);
	if (status) {
		printk(BIOS_DEBUG, "Measure CRTM Version returned 0x%x\n", status);
		return status;
	}

	status = get_intel_me_hash(hash);
	if (status) {
		printk(BIOS_DEBUG, "get_intel_me_hash returned 0x%x\n", status);
		status = TPM_E_IOERROR;
		return status;
	}

	/* Add the me hash */
	printk(BIOS_DEBUG, "%s: Add the hash returned by the ME\n",
		__func__);
	memset(&tcgEventHdr, 0, sizeof(tcgEventHdr));
	tcgEventHdr.pcrIndex  = MBOOT_PCR_INDEX_0;
	tcgEventHdr.eventType = EV_S_CRTM_CONTENTS;

	msgPtr = NULL;
	tcgEventHdr.eventSize = 0;
	status = mboot_hash_extend_log(activePcr, MBOOT_HASH_PROVIDED, hash,
			sizeof(hash), &tcgEventHdr, msgPtr, 0);
	if (status)
		printk(BIOS_DEBUG, "Add ME hash returned 0x%x\n", status);

	return status;
}
#endif // __PRE_RAM__
