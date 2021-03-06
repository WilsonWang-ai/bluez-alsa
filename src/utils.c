/*
 * BlueALSA - utils.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "utils.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <bluetooth/bluetooth.h>

#if ENABLE_LDAC
# include "ldacBT.h"
#endif

#include "a2dp-codecs.h"
#include "hfp.h"
#include "shared/defs.h"
#include "shared/log.h"

/**
 * Extract HCI device ID from the BlueZ D-Bus object path.
 *
 * @param path BlueZ D-Bus object path.
 * @return On success this function returns ID of the HCI device.
 *   Otherwise, -1 is returned. */
int g_dbus_bluez_object_path_to_hci_dev_id(const char *path) {
	if ((path = strstr(path, "/hci")) == NULL || path[4] == '\0')
		return -1;
	return atoi(&path[4]);
}

/**
 * Extract BT address from the BlueZ D-Bus object path.
 *
 * @param path BlueZ D-Bus object path.
 * @param addr Address where the parsed BT address will be stored.
 * @return On success this function returns pointer to the BT address. On
 *   error, NULL is returned. */
bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr) {

	char *tmp, *p;

	if ((path = strstr(path, "/dev_")) == NULL)
		return NULL;
	if ((tmp = strdup(path + 5)) == NULL)
		return NULL;

	for (p = tmp; *p != '\0'; p++)
		if (*p == '_')
			*p = ':';
		else if (*p == '/')
			*p = '\0';

	if (str2ba(tmp, addr) == -1)
		addr = NULL;

	free(tmp);
	return addr;
}

/**
 * Get BlueZ D-Bus object path for given transport type.
 *
 * @param type Transport type structure.
 * @return This function returns BlueZ D-Bus object path. */
const char *g_dbus_transport_type_to_bluez_object_path(struct ba_transport_type type) {
	switch (type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/Source";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "/A2DP/MPEG/Source";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "/A2DP/AAC/Source";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "/A2DP/aptX/Source";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "/A2DP/FastStream/Source";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "/A2DP/aptXHD/Source";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "/A2DP/LDAC/Source";
#endif
		default:
			warn("Unsupported A2DP codec: %#x", type.codec);
			return "/A2DP/Source";
		}
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/Sink";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "/A2DP/MPEG/Sink";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "/A2DP/AAC/Sink";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "/A2DP/FastStream/Sink";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "/A2DP/aptX/Sink";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "/A2DP/aptXHD/Sink";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "/A2DP/LDAC/Sink";
#endif
		default:
			warn("Unsupported A2DP codec: %#x", type.codec);
			return "/A2DP/Sink";
		}
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return "/HFP/HandsFree";
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return "/HFP/AudioGateway";
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "/HSP/Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "/HSP/AudioGateway";
	}
	return "/";
}

/**
 * Sanitize D-Bus object path.
 *
 * @param path D-Bus object path.
 * @return Pointer to the object path string. */
char *g_variant_sanitize_object_path(char *path) {

	char *tmp = path - 1;

	while (*(++tmp) != '\0')
		if (!(*tmp == '/' || isalnum(*tmp)))
			*tmp = '_';

	return path;
}

/**
 * Convenience wrapper around g_variant_is_of_type().
 *
 * @param value Variant for validation.
 * @param type Expected variant type.
 * @param name Variant name for logging.
 * @return If variant matches type, this function returns true. */
bool g_variant_validate_value(GVariant *value, const GVariantType *type,
		const char *name) {
	if (g_variant_is_of_type(value, type))
		return true;
	warn("Invalid variant type: %s: %s != %s", name,
			g_variant_get_type_string(value), (const char *)type);
	return false;
}

/**
 * Convert a pointer to BT address to a hash value.
 *
 * @param v A pointer to bdaddr_t structure.
 * @return Hash value compatible with GHashTable. */
unsigned int g_bdaddr_hash(const void *v) {
	const bdaddr_t *ba = (const bdaddr_t *)v;
	return ((uint32_t *)ba->b)[0] * ((uint16_t *)ba->b)[2];
}

/**
 * Compare two BT addresses.
 *
 * @param v1 A pointer to first bdaddr_t structure.
 * @param v2 A pointer to second bdaddr_t structure.
 * @return Comparision value compatible with GHashTable. */
gboolean g_bdaddr_equal(const void *v1, const void *v2) {
	return bacmp(v1, v2) == 0;
}

/**
 * Scale PCM signal stored in the buffer.
 *
 * Neutral value for scaling factor is 1.0. It is possible to increase
 * signal gain by using scaling factor values greater than 1, however
 * clipping will most certainly occur.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param size The number of samples in the buffer.
 * @param channels The number of channels in the buffer.
 * @param ch1_scale The scaling factor for 1st channel.
 * @param ch1_scale The scaling factor for 2nd channel. */
void snd_pcm_scale_s16le(int16_t *buffer, size_t size, int channels,
		double ch1_scale, double ch2_scale) {
	switch (channels) {
	case 1:
		if (ch1_scale != 1.0)
			while (size--)
				buffer[size] = buffer[size] * ch1_scale;
		break;
	case 2:
		if (ch1_scale != 1.0 || ch2_scale != 1.0)
			while (size--) {
				double scale = size % 2 == 0 ? ch1_scale : ch2_scale;
				buffer[size] = buffer[size] * scale;
			}
		break;
	}
}

/**
 * Get BlueALSA A2DP codec from string representation.
 *
 * @param codec String representation of BlueALSA audio codec.
 * @return BlueALSA audio codec or 0xFFFF for not supported value. */
uint16_t ba_transport_codecs_a2dp_from_string(const char *str) {
	if (strcmp(str, "SBC") == 0)
		return A2DP_CODEC_SBC;
#if ENABLE_MPEG
	if (strcmp(str, "MP3") == 0)
		return A2DP_CODEC_SBC;
#endif
#if ENABLE_AAC
	if (strcmp(str, "AAC") == 0)
		return A2DP_CODEC_SBC;
#endif
#if ENABLE_APTX
	if (strcmp(str, "aptX") == 0)
		return A2DP_CODEC_VENDOR_APTX;
#endif
#if ENABLE_FASTSTREAM
	if (strcmp(str, "FastStream") == 0)
		return A2DP_CODEC_VENDOR_FASTSTREAM;
#endif
#if ENABLE_APTX_HD
	if (strcmp(str, "aptX-HD") == 0)
		return A2DP_CODEC_VENDOR_APTX_HD;
#endif
#if ENABLE_LDAC
	if (strcmp(str, "LDAC") == 0)
		return A2DP_CODEC_VENDOR_LDAC;
#endif
	return 0xFFFF;
}

/**
 * Convert BlueALSA A2DP codec into a human-readable string.
 *
 * @param codec BlueALSA A2DP audio codec.
 * @return Human-readable string or NULL for unknown codec. */
const char *ba_transport_codecs_a2dp_to_string(uint16_t codec) {
	switch (codec) {
	case A2DP_CODEC_SBC:
		return "SBC";
	case A2DP_CODEC_MPEG12:
		return "MP3";
	case A2DP_CODEC_MPEG24:
		return "AAC";
	case A2DP_CODEC_ATRAC:
		return "ATRAC";
	case A2DP_CODEC_VENDOR_APTX:
		return "aptX";
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		return "FastStream";
	case A2DP_CODEC_VENDOR_APTX_LL:
		return "aptX-LL";
	case A2DP_CODEC_VENDOR_APTX_HD:
		return "aptX-HD";
	case A2DP_CODEC_VENDOR_LDAC:
		return "LDAC";
	case A2DP_CODEC_VENDOR_LHDC:
		return "LHDC";
	case A2DP_CODEC_VENDOR_LHDC_V1:
		return "LHDCv1";
	case A2DP_CODEC_VENDOR_LLAC:
		return "LLAC";
	case A2DP_CODEC_VENDOR_SAMSUNG_HD:
		return "samsung-HD";
	case A2DP_CODEC_VENDOR_SAMSUNG_SC:
		return "samsung-SC";
	default:
		return NULL;
	}
}

/**
 * Get BlueALSA HFP codec from string representation.
 *
 * @param codec String representation of BlueALSA audio codec.
 * @return BlueALSA audio codec or 0xFFFF for not supported value. */
uint16_t ba_transport_codecs_hfp_from_string(const char *str) {
	if (strcmp(str, "CVSD") == 0)
		return HFP_CODEC_CVSD;
#if ENABLE_MSBC
	if (strcmp(str, "mSBC") == 0)
		return HFP_CODEC_MSBC;
#endif
	return 0xFFFF;
}

/**
 * Convert HFP audio codec into a human-readable string.
 *
 * @param codec HFP audio codec.
 * @return Human-readable string or NULL for unknown codec. */
const char *ba_transport_codecs_hfp_to_string(uint16_t codec) {
	switch (codec) {
	case HFP_CODEC_CVSD:
		return "CVSD";
	case HFP_CODEC_MSBC:
		return "mSBC";
	default:
		return NULL;
	}
}

/**
 * Convert BlueALSA transport type into a human-readable string.
 *
 * @param type Transport type structure.
 * @return Human-readable string. */
const char *ba_transport_type_to_string(struct ba_transport_type type) {
	switch (type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Source (SBC)";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "A2DP Source (MP3)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Source (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Source (aptX)";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "A2DP Source (FastStream)";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "A2DP Source (aptX HD)";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "A2DP Source (LDAC)";
#endif
		default:
			return "A2DP Source";
		}
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Sink (SBC)";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "A2DP Sink (MP3)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Sink (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Sink (aptX)";
#endif
#if ENABLE_FASTSTREAM
		case A2DP_CODEC_VENDOR_FASTSTREAM:
			return "A2DP Sink (FastStream)";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "A2DP Sink (aptX HD)";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "A2DP Sink (LDAC)";
#endif
		default:
			return "A2DP Sink";
		}
	case BA_TRANSPORT_PROFILE_HFP_HF:
		switch (type.codec) {
		case HFP_CODEC_CVSD:
			return "HFP Hands-Free (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Hands-Free (mSBC)";
		default:
			return "HFP Hands-Free";
		}
	case BA_TRANSPORT_PROFILE_HFP_AG:
		switch (type.codec) {
		case HFP_CODEC_CVSD:
			return "HFP Audio Gateway (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Audio Gateway (mSBC)";
		default:
			return "HFP Audio Gateway";
		}
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "HSP Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "HSP Audio Gateway";
	}
	debug("Unknown transport type: %#x %#x", type.profile, type.codec);
	return "N/A";
}

#if ENABLE_MP3LAME
/**
 * Get maximum possible bit-rate for the given bit-rate mask.
 *
 * @param mask MPEG-1 layer III bit-rate mask.
 * @return Bit-rate in kilobits per second. */
int a2dp_mpeg1_mp3_get_max_bitrate(uint16_t mask) {

	static int bitrates[] = { 320, 256, 224, 192, 160, 128, 112, 96, 80, 64, 56, 48, 40, 32 };
	size_t i = 0;

	while (i < ARRAYSIZE(bitrates)) {
		if (mask & (1 << (14 - i)))
			return bitrates[i];
		i++;
	}

	return -1;
}
#endif

#if ENABLE_MP3LAME
/**
 * Get string representation of LAME error code.
 *
 * @param error LAME encoder error code.
 * @return Human-readable string. */
const char *lame_encode_strerror(int err) {
	switch (err) {
	case -1:
		return "Too small output buffer";
	case -2:
		return "Out of memory";
	case -3:
		return "Params not initialized";
	case -4:
		return "Psycho acoustic error";
	default:
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC decoder error code.
 *
 * @param err FDK-AAC decoder error code.
 * @return Human-readable string. */
const char *aacdec_strerror(AAC_DECODER_ERROR err) {
	switch (err) {
	case AAC_DEC_OK:
		return "Success";
	case AAC_DEC_OUT_OF_MEMORY:
		return "Out of memory";
	case AAC_DEC_TRANSPORT_SYNC_ERROR:
		return "Transport sync error";
	case AAC_DEC_NOT_ENOUGH_BITS:
		return "Not enough bits";
	case AAC_DEC_INVALID_HANDLE:
		return "Invalid handle";
	case AAC_DEC_UNSUPPORTED_AOT:
		return "Unsupported AOT";
	case AAC_DEC_UNSUPPORTED_FORMAT:
		return "Unsupported format";
	case AAC_DEC_UNSUPPORTED_ER_FORMAT:
		return "Unsupported ER format";
	case AAC_DEC_UNSUPPORTED_EPCONFIG:
		return "Unsupported EP format";
	case AAC_DEC_UNSUPPORTED_MULTILAYER:
		return "Unsupported multilayer";
	case AAC_DEC_UNSUPPORTED_CHANNELCONFIG:
		return "Unsupported channels";
	case AAC_DEC_UNSUPPORTED_SAMPLINGRATE:
		return "Unsupported sample rate";
	case AAC_DEC_INVALID_SBR_CONFIG:
		return "Unsupported SBR";
	case AAC_DEC_SET_PARAM_FAIL:
		return "Unsupported parameter";
	case AAC_DEC_NEED_TO_RESTART:
		return "Restart required";
	case AAC_DEC_TRANSPORT_ERROR:
		return "Transport error";
	case AAC_DEC_PARSE_ERROR:
		return "Parse error";
	case AAC_DEC_UNSUPPORTED_EXTENSION_PAYLOAD:
		return "Unsupported extension payload";
	case AAC_DEC_DECODE_FRAME_ERROR:
		return "Bitstream corrupted";
	case AAC_DEC_CRC_ERROR:
		return "CRC mismatch";
	case AAC_DEC_INVALID_CODE_BOOK:
		return "Invalid codebook";
	case AAC_DEC_UNSUPPORTED_PREDICTION:
		return "Unsupported prediction";
	case AAC_DEC_UNSUPPORTED_CCE:
		return "Unsupported CCE";
	case AAC_DEC_UNSUPPORTED_LFE:
		return "Unsupported LFE";
	case AAC_DEC_UNSUPPORTED_GAIN_CONTROL_DATA:
		return "Unsupported gain control data";
	case AAC_DEC_UNSUPPORTED_SBA:
		return "Unsupported SBA";
	case AAC_DEC_TNS_READ_ERROR:
		return "TNS read error";
	case AAC_DEC_RVLC_ERROR:
		return "RVLC decode error";
	case AAC_DEC_ANC_DATA_ERROR:
		return "Ancillary data error";
	case AAC_DEC_TOO_SMALL_ANC_BUFFER:
		return "Too small ancillary buffer";
	case AAC_DEC_TOO_MANY_ANC_ELEMENTS:
		return "Too many ancillary elements";
	default:
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC encoder error code.
 *
 * @param err FDK-AAC encoder error code.
 * @return Human-readable string. */
const char *aacenc_strerror(AACENC_ERROR err) {
	switch (err) {
	case AACENC_OK:
		return "Success";
	case AACENC_INVALID_HANDLE:
		return "Invalid handle";
	case AACENC_MEMORY_ERROR:
		return "Out of memory";
	case AACENC_UNSUPPORTED_PARAMETER:
		return "Unsupported parameter";
	case AACENC_INVALID_CONFIG:
		return "Invalid config";
	case AACENC_INIT_ERROR:
		return "Initialization error";
	case AACENC_INIT_AAC_ERROR:
		return "AAC library initialization error";
	case AACENC_INIT_SBR_ERROR:
		return "SBR library initialization error";
	case AACENC_INIT_TP_ERROR:
		return "Transport library initialization error";
	case AACENC_INIT_META_ERROR:
		return "Metadata library initialization error";
	case AACENC_ENCODE_ERROR:
		return "Encoding error";
	case AACENC_ENCODE_EOF:
		return "End of file";
	default:
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif

#if ENABLE_LDAC
/**
 * Get string representation of the LDAC error code.
 *
 * @param err LDAC error code.
 * @return Human-readable string. */
const char *ldacBT_strerror(int err) {
	switch (LDACBT_API_ERR(err)) {
	case LDACBT_ERR_NONE:
		return "Success";
	case LDACBT_ERR_ASSERT_SAMPLING_FREQ:
	case LDACBT_ERR_ASSERT_SUP_SAMPLING_FREQ:
	case LDACBT_ERR_CHECK_SAMPLING_FREQ:
		return "Invalid sample rate";
	case LDACBT_ERR_ASSERT_CHANNEL_CONFIG:
	case LDACBT_ERR_CHECK_CHANNEL_CONFIG:
		return "Invalid channel config";
	case LDACBT_ERR_ASSERT_FRAME_LENGTH:
	case LDACBT_ERR_ASSERT_SUP_FRAME_LENGTH:
	case LDACBT_ERR_ASSERT_FRAME_STATUS:
		return "Invalid frame status";
	case LDACBT_ERR_ASSERT_NSHIFT:
		return "Invalid N-shift";
	case LDACBT_ERR_ASSERT_CHANNEL_MODE:
		return "Invalid channel mode";
	case LDACBT_ERR_ALTER_EQMID_LIMITED:
		return "EQMID limited";
	case LDACBT_ERR_HANDLE_NOT_INIT:
		return "Invalid handle";
	case LDACBT_ERR_ILL_EQMID:
		return "Unsupported EQMID";
	case LDACBT_ERR_ILL_SAMPLING_FREQ:
		return "Unsupported sample rate";
	case LDACBT_ERR_ILL_NUM_CHANNEL:
		return "Unsupported channels";
	case LDACBT_ERR_ILL_MTU_SIZE:
		return "Unsupported MTU";
	default:
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif
