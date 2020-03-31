/*
 * BlueALSA - aplay.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "alsa-pcm.h"

struct pcm_worker {
	pthread_t thread;
	/* used BlueALSA PCM device */
	struct ba_pcm ba_pcm;
	/* file descriptor of PCM FIFO */
	int ba_pcm_fd;
	/* file descriptor of PCM control */
	int ba_pcm_ctrl_fd;
	/* opened playback PCM device */
	snd_pcm_t *pcm;
	/* mixer for volume control */
	snd_mixer_t *mixer;
	snd_mixer_elem_t *mixer_elem;
	/* if true, playback is active */
	bool active;
	/* human-readable BT address */
	char addr[18];
};

static unsigned int verbose = 0;
static bool list_bt_devices = false;
static bool list_bt_pcms = false;
static const char *pcm_device = "default";
static const char *mixer_device = "default";
static const char *mixer_elem_name = "Master";
static bool ba_profile_a2dp = true;
static bool ba_addr_any = false;
static bdaddr_t *ba_addrs = NULL;
static size_t ba_addrs_count = 0;
static unsigned int pcm_buffer_time = 500000;
static unsigned int pcm_period_time = 100000;
static bool pcm_mixer = true;

static struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;

static struct ba_pcm *ba_pcms = NULL;
static size_t ba_pcms_count = 0;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct pcm_worker *workers = NULL;
static size_t workers_count = 0;
static size_t workers_size = 0;

static bool main_loop_on = true;
static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	main_loop_on = false;
}

static snd_pcm_format_t get_snd_pcm_format(uint16_t format) {
	switch (format) {
	case 0x0008:
		return SND_PCM_FORMAT_U8;
	case 0x8010:
		return SND_PCM_FORMAT_S16_LE;
	case 0x8018:
		return SND_PCM_FORMAT_S24_3LE;
	default:
		error("Unsupported PCM format: %#x", format);
		return SND_PCM_FORMAT_UNKNOWN;
	}
}

static int mixer_open(snd_mixer_t **mixer, snd_mixer_elem_t **elem, char **msg) {

	snd_mixer_t *_mixer = NULL;
	snd_mixer_elem_t *_elem;
	char buf[256];
	int err;

	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_index(id, 0);
	snd_mixer_selem_id_set_name(id, mixer_elem_name);

	if ((err = snd_mixer_open(&_mixer, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Open mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_attach(_mixer, mixer_device)) != 0) {
		snprintf(buf, sizeof(buf), "Attach mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_selem_register(_mixer, NULL, NULL)) != 0) {
		snprintf(buf, sizeof(buf), "Register mixer class: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_load(_mixer)) != 0) {
		snprintf(buf, sizeof(buf), "Load mixer elements: %s", snd_strerror(err));
		goto fail;
	}
	if ((_elem = snd_mixer_find_selem(_mixer, id)) == NULL) {
		snprintf(buf, sizeof(buf), "Mixer element not found");
		err = -1;
		goto fail;
	}

	*mixer = _mixer;
	*elem = _elem;
	return 0;

fail:
	if (_mixer != NULL)
		snd_mixer_close(_mixer);
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static struct ba_pcm *get_ba_pcm(const char *path) {

	size_t i;

	for (i = 0; i < ba_pcms_count; i++)
		if (strcmp(ba_pcms[i].pcm_path, path) == 0)
			return &ba_pcms[i];

	return NULL;
}

static struct pcm_worker *get_active_worker(void) {

	struct pcm_worker *w = NULL;
	size_t i;

	pthread_rwlock_rdlock(&workers_lock);

	for (i = 0; i < workers_count; i++)
		if (workers[i].active) {
			w = &workers[i];
			break;
		}

	pthread_rwlock_unlock(&workers_lock);

	return w;
}

static int disable_bluealsa_pcm_software_volume(
		struct ba_pcm *ba_pcm,
		DBusError *err) {

	if (!ba_pcm->soft_volume)
		return 0;

	ba_pcm->soft_volume = false;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, ba_pcm, BLUEALSA_PCM_SOFT_VOLUME, err))
		return -1;

	return 0;
}

static int pause_device_player(const struct ba_pcm *ba_pcm) {

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	char path[160];
	int ret = 0;

	snprintf(path, sizeof(path), "%s/player0", ba_pcm->device_path);
	msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.MediaPlayer1", "Pause");

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn, msg,
					DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		warn("Couldn't pause player: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	debug("Requested playback pause");
	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return ret;
}

static int pcm_worker_update_mixer_volume(struct pcm_worker *worker) {

	long min, max, v;
	int err;

	if (worker->mixer_elem == NULL)
		return 0;

	snd_mixer_selem_get_playback_volume_range(worker->mixer_elem, &min, &max);

	v = max * worker->ba_pcm.volume.ch1_volume / 127;
	err = snd_mixer_selem_set_playback_volume(worker->mixer_elem, 0, v);
	debug("%d: %ld %ld %ld", err, min, v, max);

	v = max * worker->ba_pcm.volume.ch2_volume / 127;
	err = snd_mixer_selem_set_playback_volume(worker->mixer_elem, 1, v);
	debug("%d: %ld", err, v);

	return 0;
}

static void pcm_worker_routine_exit(struct pcm_worker *worker) {
	if (worker->ba_pcm_fd != -1) {
		close(worker->ba_pcm_fd);
		worker->ba_pcm_fd = -1;
	}
	if (worker->ba_pcm_ctrl_fd != -1) {
		close(worker->ba_pcm_ctrl_fd);
		worker->ba_pcm_ctrl_fd = -1;
	}
	if (worker->pcm != NULL) {
		snd_pcm_close(worker->pcm);
		worker->pcm = NULL;
	}
	if (worker->mixer != NULL) {
		snd_mixer_close(worker->mixer);
		worker->mixer_elem = NULL;
		worker->mixer = NULL;
	}
	debug("Exiting PCM worker %s", worker->addr);
}

static void *pcm_worker_routine(struct pcm_worker *w) {

	snd_pcm_format_t pcm_format = get_snd_pcm_format(w->ba_pcm.format);
	ssize_t pcm_format_size = snd_pcm_format_size(pcm_format, 1);
	size_t pcm_1s_samples = w->ba_pcm.sampling * w->ba_pcm.channels;
	ffb_uint8_t buffer = { 0 };

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_cleanup_push(PTHREAD_CLEANUP(pcm_worker_routine_exit), w);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &buffer);

	/* create buffer big enough to hold 100 ms of PCM data */
	if (ffb_init(&buffer, pcm_1s_samples / 10 * pcm_format_size) == NULL) {
		error("Couldn't create PCM buffer: %s", strerror(ENOMEM));
		goto fail;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_open_pcm(&dbus_ctx, w->ba_pcm.pcm_path,
				&w->ba_pcm_fd, &w->ba_pcm_ctrl_fd, &err)) {
		error("Couldn't open PCM: %s", err.message);
		dbus_error_free(&err);
		goto fail;
	}

	/* Try to disable BlueALSA internal software volume control, so we
	 * could forward volume level to ALSA playback PCM mixer. */
	if (disable_bluealsa_pcm_software_volume(&w->ba_pcm, &err) == -1) {
		warn("Couldn't disable BlueALSA software volume: %s", err.message);
		dbus_error_free(&err);
	}

	/* Initialize the max read length to 10 ms. Later, when the PCM device
	 * will be opened, this value will be adjusted to one period size. */
	size_t pcm_max_read_len = pcm_1s_samples / 100;
	size_t pcm_open_retries = 0;

	/* These variables determine how and when the pause command will be send
	 * to the device player. In order not to flood BT connection with AVRCP
	 * packets, we are going to send pause command every 0.5 second. */
	size_t pause_threshold = pcm_1s_samples / 2 * pcm_format_size;
	size_t pause_counter = 0;
	size_t pause_bytes = 0;

	struct pollfd pfds[] = {{ w->ba_pcm_fd, POLLIN, 0 }};
	int timeout = -1;

	debug("Starting PCM loop");
	while (main_loop_on) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		ssize_t ret;

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */
		switch (poll(pfds, ARRAYSIZE(pfds), timeout)) {
		case -1:
			if (errno == EINTR)
				continue;
			error("PCM FIFO poll error: %s", strerror(errno));
			goto fail;
		case 0:
			debug("Device marked as inactive: %s", w->addr);
			pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
			pcm_max_read_len = pcm_1s_samples / 100;
			pause_counter = pause_bytes = 0;
			ffb_rewind(&buffer);
			if (w->pcm != NULL) {
				snd_pcm_close(w->pcm);
				w->pcm = NULL;
			}
			w->active = false;
			timeout = -1;
			continue;
		}

		/* FIFO has been terminated on the writing side */
		if (pfds[0].revents & POLLHUP)
			break;

		#define MIN(a,b) a < b ? a : b
		size_t _in = MIN(pcm_max_read_len, ffb_len_in(&buffer));
		if ((ret = read(w->ba_pcm_fd, buffer.tail, _in * pcm_format_size)) == -1) {
			if (errno == EINTR)
				continue;
			error("PCM FIFO read error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/* If PCM mixer is disabled, check whether we should play audio. */
		if (!pcm_mixer) {
			struct pcm_worker *worker = get_active_worker();
			if (worker != NULL && worker != w) {
				if (pause_counter < 5 && (pause_bytes += ret) > pause_threshold) {
					if (pause_device_player(&w->ba_pcm) == -1)
						/* pause command does not work, stop further requests */
						pause_counter = 5;
					pause_counter++;
					pause_bytes = 0;
					timeout = 100;
				}
				continue;
			}
		}

		if (w->pcm == NULL) {

			unsigned int buffer_time = pcm_buffer_time;
			unsigned int period_time = pcm_period_time;
			snd_pcm_uframes_t buffer_size;
			snd_pcm_uframes_t period_size;
			char *tmp;

			/* After PCM open failure wait one second before retry. This can not be
			 * done with a single sleep() call, because we have to drain PCM FIFO. */
			if (pcm_open_retries++ % 20 != 0) {
				usleep(50000);
				continue;
			}

			if (alsa_pcm_open(&w->pcm, pcm_device, pcm_format, w->ba_pcm.channels,
						w->ba_pcm.sampling, &buffer_time, &period_time, &tmp) != 0) {
				warn("Couldn't open PCM: %s", tmp);
				pcm_max_read_len = buffer.size;
				usleep(50000);
				free(tmp);
				continue;
			}

			if (w == &workers[0]) {
				if (mixer_open(&w->mixer, &w->mixer_elem, &tmp) != 0) {
					warn("Couldn't open mixer: %s", tmp);
					free(tmp);
				}
				pcm_worker_update_mixer_volume(w);
			}

			snd_pcm_get_params(w->pcm, &buffer_size, &period_size);
			pcm_max_read_len = period_size * w->ba_pcm.channels;
			pcm_open_retries = 0;

			if (verbose >= 2) {
				printf("Used configuration for %s:\n"
						"  PCM buffer time: %u us (%zu bytes)\n"
						"  PCM period time: %u us (%zu bytes)\n"
						"  PCM format: %s\n"
						"  Sampling rate: %u Hz\n"
						"  Channels: %u\n",
						w->addr,
						buffer_time, snd_pcm_frames_to_bytes(w->pcm, buffer_size),
						period_time, snd_pcm_frames_to_bytes(w->pcm, period_size),
						snd_pcm_format_name(pcm_format),
						w->ba_pcm.sampling,
						w->ba_pcm.channels);
			}

		}

		/* mark device as active and set timeout to 500ms */
		w->active = true;
		timeout = 500;

		/* calculate the overall number of frames in the buffer */
		ffb_seek(&buffer, ret / pcm_format_size);
		snd_pcm_sframes_t frames = ffb_len_out(&buffer) / w->ba_pcm.channels;

		if ((frames = snd_pcm_writei(w->pcm, buffer.data, frames)) < 0)
			switch (-frames) {
			case EPIPE:
				debug("An underrun has occurred");
				snd_pcm_prepare(w->pcm);
				usleep(50000);
				frames = 0;
				break;
			default:
				error("Couldn't write to PCM: %s", snd_strerror(frames));
				goto fail;
			}

		/* move leftovers to the beginning and reposition tail */
		ffb_shift(&buffer, frames * w->ba_pcm.channels);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static int supervise_pcm_worker_start(struct ba_pcm *ba_pcm) {

	size_t i;
	for (i = 0; i < workers_count; i++)
		if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0)
			return 0;

	pthread_rwlock_wrlock(&workers_lock);

	workers_count++;
	if (workers_size < workers_count) {
		struct pcm_worker *tmp = workers;
		workers_size += 4;  /* coarse-grained realloc */
		if ((workers = realloc(workers, sizeof(*workers) * workers_size)) == NULL) {
			error("Couldn't (re)allocate memory for PCM workers: %s", strerror(ENOMEM));
			workers = tmp;
			pthread_rwlock_unlock(&workers_lock);
			return -1;
		}
	}

	struct pcm_worker *worker = &workers[workers_count - 1];
	memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
	ba2str(&worker->ba_pcm.addr, worker->addr);
	worker->active = false;
	worker->ba_pcm_fd = -1;
	worker->ba_pcm_ctrl_fd = -1;
	worker->pcm = NULL;

	pthread_rwlock_unlock(&workers_lock);

	debug("Creating PCM worker %s", worker->addr);

	if ((errno = pthread_create(&worker->thread, NULL,
					PTHREAD_ROUTINE(pcm_worker_routine), worker)) != 0) {
		error("Couldn't create PCM worker %s: %s", worker->addr, strerror(errno));
		workers_count--;
		return -1;
	}

	return 0;
}

static int supervise_pcm_worker_stop(struct ba_pcm *ba_pcm) {

	size_t i;
	for (i = 0; i < workers_count; i++)
		if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
			pthread_rwlock_wrlock(&workers_lock);
			pthread_cancel(workers[i].thread);
			pthread_join(workers[i].thread, NULL);
			memcpy(&workers[i], &workers[--workers_count], sizeof(workers[i]));
			pthread_rwlock_unlock(&workers_lock);
		}

	return 0;
}

static int supervise_pcm_worker(struct ba_pcm *ba_pcm) {

	if (ba_pcm == NULL)
		return -1;

	if (!(ba_pcm->flags & BA_PCM_FLAG_SOURCE))
		goto stop;

	if ((ba_profile_a2dp && !(ba_pcm->flags & BA_PCM_FLAG_PROFILE_A2DP)) ||
			(!ba_profile_a2dp && !(ba_pcm->flags & BA_PCM_FLAG_PROFILE_SCO)))
		goto stop;

	/* check whether SCO has selected codec */
	if (ba_pcm->flags & BA_PCM_FLAG_PROFILE_SCO &&
			ba_pcm->codec == 0) {
		debug("Skipping SCO with codec not selected");
		goto stop;
	}

	if (ba_addr_any)
		goto start;

	size_t i;
	for (i = 0; i < ba_addrs_count; i++)
		if (bacmp(&ba_addrs[i], &ba_pcm->addr) == 0)
			goto start;

stop:
	return supervise_pcm_worker_stop(ba_pcm);
start:
	return supervise_pcm_worker_start(ba_pcm);
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;
	struct ba_pcm *pcm;

	if (strcmp(interface, BLUEALSA_INTERFACE_MANAGER) == 0) {

		if (strcmp(signal, "PCMAdded") == 0) {
			struct ba_pcm *tmp = ba_pcms;
			if ((ba_pcms = realloc(ba_pcms, (ba_pcms_count + 1) * sizeof(*ba_pcms))) == NULL) {
				error("Couldn't add new PCM: %s", strerror(ENOMEM));
				ba_pcms = tmp;
				goto fail;
			}
			if (!dbus_message_iter_init(message, &iter) ||
					!bluealsa_dbus_message_iter_get_pcm(&iter, NULL, &ba_pcms[ba_pcms_count])) {
				error("Couldn't add new PCM: %s", "Invalid signal signature");
				goto fail;
			}
			supervise_pcm_worker(&ba_pcms[ba_pcms_count++]);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (strcmp(signal, "PCMRemoved") == 0) {
			if (!dbus_message_iter_init(message, &iter) ||
					dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
				error("Couldn't remove PCM: %s", "Invalid signal signature");
				goto fail;
			}
			dbus_message_iter_get_basic(&iter, &path);
			supervise_pcm_worker_stop(get_ba_pcm(path));
			return DBUS_HANDLER_RESULT_HANDLED;
		}

	}

	if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0) {
		if ((pcm = get_ba_pcm(path)) == NULL)
			goto fail;
		if (!dbus_message_iter_init(message, &iter) ||
				dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
			error("Couldn't update PCM: %s", "Invalid signal signature");
			goto fail;
		}
		dbus_message_iter_get_basic(&iter, &interface);
		dbus_message_iter_next(&iter);
		if (!bluealsa_dbus_message_iter_get_pcm_props(&iter, NULL, pcm))
			goto fail;
		supervise_pcm_worker(pcm);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVvlLb:d:m:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "list-devices", no_argument, NULL, 'l' },
		{ "list-pcms", no_argument, NULL, 'L' },
		{ "dbus", required_argument, NULL, 'b' },
		{ "pcm", required_argument, NULL, 'd' },
		{ "pcm-buffer-time", required_argument, NULL, 3 },
		{ "pcm-period-time", required_argument, NULL, 4 },
		{ "mixer-device", required_argument, NULL, 'm' },
		{ "mixer-name", required_argument, NULL, 6 },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
		{ "single-audio", no_argument, NULL, 5 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("Usage:\n"
					"  %s [OPTION]... <BT-ADDR>...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -v, --verbose\t\tmake output more verbose\n"
					"  -l, --list-devices\tlist all connected BT devices\n"
					"  -L, --list-pcms\tlist all available BT PCMs\n"
					"  -b, --dbus=NAME\tBlueALSA service name suffix\n"
					"  -d, --pcm=NAME\tplayback PCM device to use\n"
					"  --pcm-buffer-time=INT\tplayback PCM buffer time\n"
					"  --pcm-period-time=INT\tplayback PCM period time\n"
					"  -m, --mixer=NAME\tmixer device to use\n"
					"  --mixer-name=NAME\tmixer element name\n"
					"  --profile-a2dp\tuse A2DP profile (default)\n"
					"  --profile-sco\t\tuse SCO profile\n"
					"  --single-audio\tsingle audio mode\n"
					"\nNote:\n"
					"If one wants to receive audio from more than one Bluetooth device, it is\n"
					"possible to specify more than one MAC address. By specifying any/empty MAC\n"
					"address (00:00:00:00:00:00), one will allow connections from any Bluetooth\n"
					"device.\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'v' /* --verbose */ :
			verbose++;
			break;

		case 'l' /* --list-devices */ :
			list_bt_devices = true;
			break;
		case 'L' /* --list-pcms */ :
			list_bt_pcms = true;
			break;

		case 'b' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			break;

		case 'd' /* --pcm=NAME */ :
			pcm_device = optarg;
			break;
		case 3 /* --pcm-buffer-time=INT */ :
			pcm_buffer_time = atoi(optarg);
			break;
		case 4 /* --pcm-period-time=INT */ :
			pcm_period_time = atoi(optarg);
			break;

		case 'm' /* --mixer-device=NAME */ :
			mixer_device = optarg;
			break;
		case 6 /* --mixer-name=NAME */ :
			mixer_elem_name = optarg;
			break;

		case 1 /* --profile-a2dp */ :
			ba_profile_a2dp = true;
			break;
		case 2 /* --profile-sco */ :
			ba_profile_a2dp = false;
			break;

		case 5 /* --single-audio */ :
			pcm_mixer = false;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind == argc &&
			!(list_bt_devices || list_bt_pcms))
		goto usage;

	log_open(argv[0], false, false);
	dbus_threads_init_default();

	size_t i;

	ba_addrs_count = argc - optind;
	if ((ba_addrs = malloc(sizeof(*ba_addrs) * ba_addrs_count)) == NULL) {
		error("Couldn't allocate memory for BT addresses");
		return EXIT_FAILURE;
	}
	for (i = 0; i < ba_addrs_count; i++) {
		if (str2ba(argv[i + optind], &ba_addrs[i]) != 0) {
			error("Invalid BT device address: %s", argv[i + optind]);
			return EXIT_FAILURE;
		}
		if (bacmp(&ba_addrs[i], BDADDR_ANY) == 0)
			ba_addr_any = true;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	if (list_bt_devices || list_bt_pcms) {

		const char *tmp;
		size_t i;

		if (!bluealsa_dbus_get_pcms(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
			warn("Couldn't get BlueALSA PCM list: %s", err.message);

		printf("**** List of PLAYBACK Bluetooth Devices ****\n");
		for (i = 0, tmp = NULL; i < ba_pcms_count; i++)
			if (ba_pcms[i].flags & BA_PCM_FLAG_SINK &&
					(tmp == NULL || strcmp(ba_pcms[i].device_path, tmp) != 0)) {
				printf("%s\n", ba_pcms[i].device_path);
				tmp = ba_pcms[i].device_path;
			}

		printf("**** List of CAPTURE Bluetooth Devices ****\n");
		for (i = 0, tmp = NULL; i < ba_pcms_count; i++)
			if (ba_pcms[i].flags & BA_PCM_FLAG_SINK &&
					(tmp == NULL || strcmp(ba_pcms[i].device_path, tmp) != 0)) {
				printf("%s\n", ba_pcms[i].device_path);
				tmp = ba_pcms[i].device_path;
			}

		return EXIT_SUCCESS;
	}

	if (verbose >= 1) {

		char *ba_str = malloc(19 * ba_addrs_count + 1);
		char *tmp = ba_str;
		size_t i;

		for (i = 0; i < ba_addrs_count; i++, tmp += 19)
			ba2str(&ba_addrs[i], stpcpy(tmp, ", "));

		printf("Selected configuration:\n"
				"  BlueALSA service: %s\n"
				"  PCM device: %s\n"
				"  PCM buffer time: %u us\n"
				"  PCM period time: %u us\n"
				"  ALSA mixer device: %s\n"
				"  ALSA mixer element: %s\n"
				"  Bluetooth device(s): %s\n"
				"  Profile: %s\n",
				dbus_ba_service,
				pcm_device, pcm_buffer_time, pcm_period_time,
				mixer_device, mixer_elem_name,
				ba_addr_any ? "ANY" : &ba_str[2],
				ba_profile_a2dp ? "A2DP" : "SCO");

		free(ba_str);
	}

	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMAdded", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMRemoved", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
			"arg0='"BLUEALSA_INTERFACE_PCM"'");

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		error("Couldn't add D-Bus filter: %s", err.message);
		return EXIT_FAILURE;
	}

	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
		warn("Couldn't get BlueALSA PCM list: %s", err.message);

	for (i = 0; i < ba_pcms_count; i++)
		supervise_pcm_worker(&ba_pcms[i]);

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	debug("Starting main loop");
	while (main_loop_on) {

		struct pollfd pfds[10];
		nfds_t pfds_len = ARRAYSIZE(pfds);

		if (!bluealsa_dbus_connection_poll_fds(&dbus_ctx, pfds, &pfds_len)) {
			error("Couldn't get D-Bus connection file descriptors");
			return EXIT_FAILURE;
		}

		if (poll(pfds, pfds_len, -1) == -1 &&
				errno == EINTR)
			continue;

		if (bluealsa_dbus_connection_poll_dispatch(&dbus_ctx, pfds, pfds_len))
			while (dbus_connection_dispatch(dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
				continue;

	}

	return EXIT_SUCCESS;
}
