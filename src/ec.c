// ec - echo canceller

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#include <speex/speex_echo.h>

#include "conf.h"
#include "audio.h"

const char *usage =
    "Usage:\n %s [options]\n"
    "Options:\n"
    " -i PCM            playback PCM (default)\n"
    " -o PCM            capture PCM (default)\n"
    " -r rate           sample rate (16000)\n"
    " -c channels       recording channels (2)\n"
    " -b size           buffer size (262144)\n"
    " -d delay          system delay between playback and capture (0)\n"
    " -f filter_length  AEC filter length (2048)\n"
    " -s                save audio to /tmp/playback.raw, /tmp/recording.raw and /tmp/out.raw\n"
    " -D                daemonize\n"
    " -h                display this help text\n"
    "Note:\n"
    " Access audio I/O through named pipes (/tmp/ec.input for playback and /tmp/ec.output for recording)\n"
    "  `cat audio.raw > /tmp/ec.input` to play audio\n"
    "  `cat /tmp/ec.output > out.raw` to get recording audio\n"
    " Only support mono playback\n";

volatile int g_is_quit = 0;

extern int fifo_setup(PaUtilRingBuffer *capture);

void int_handler(int signal)
{
    printf("Caught signal %d, quit...\n", signal);

    g_is_quit = 1;
}

int main(int argc, char *argv[])
{
    SpeexEchoState *echo_state;
    int16_t *rec = NULL;
    int16_t *far = NULL;
    int16_t *out = NULL;
    FILE *fp_rec = NULL;
    FILE *fp_far = NULL;
    FILE *fp_out = NULL;

    int opt = 0;
    int delay = 0;
    int save_audio = 0;
    int daemonize = 0;

    conf_t config = {
        .rec_pcm = "default",
        .out_pcm = "default",
        .playback_fifo = "/tmp/ec.input",
        .out_fifo = "/tmp/ec.output",
        .rate = 16000,
        .rec_channels = 2,
        .out_channels = 1,
        .bits_per_sample = 16,
        .buffer_size = 1024 * 16,
        .playback_fifo_size = 1024 * 4,
        .filter_length = 2048,
        .bypass = 1};

    while ((opt = getopt(argc, argv, "b:c:d:Df:hi:o:r:s")) != -1)
    {
        switch (opt)
        {
        case 'b':
            config.buffer_size = atoi(optarg);
            break;
        case 'c':
            config.rec_channels = atoi(optarg);
            break;
        case 'd':
            delay = atoi(optarg);
            break;
        case 'D':
            daemonize = 1;
            break;
        case 'f':
            config.filter_length = atoi(optarg);
            break;
        case 'h':
            printf(usage, argv[0]);
            exit(0);
        case 'i':
            config.rec_pcm = optarg;
            break;
        case 'o':
            config.out_pcm = optarg;
            break;
        case 'r':
            config.rate = atoi(optarg);
            break;
        case 's':
            save_audio = 1;
            break;
        case '?':
            printf("\n");
            printf(usage, argv[0]);
            exit(1);
        default:
            break;
        }
    }

    if (daemonize)
    {
        pid_t pid, sid;

        /* Fork off the parent process */
        pid = fork();
        if (pid < 0)
        {
            printf("fork() failed\n");
            exit(1);
        }
        /* If we got a good PID, then
           we can exit the parent process. */
        if (pid > 0)
        {
            exit(0);
        }

        /* Change the file mode mask */
        umask(0);

        /* Open any logs here */

        /* Create a new SID for the child process */
        sid = setsid();
        if (sid < 0)
        {
            printf("setsid() failed\n");
            exit(1);
        }

        /* Change the current working directory */
        if ((chdir("/")) < 0)
        {
            printf("chdir() failed\n");
            exit(1);
        }
    }

    int frame_size = config.rate * 10 / 1000; // 10 ms

    if (save_audio)
    {
        fp_far = fopen("/tmp/playback.raw", "wb");
        fp_rec = fopen("/tmp/recording.raw", "wb");
        fp_out = fopen("/tmp/out.raw", "wb");

        if (fp_far == NULL || fp_rec == NULL || fp_out == NULL)
        {
            printf("Fail to open file(s)\n");
            exit(1);
        }
    }

    rec = (int16_t *)calloc(frame_size * config.rec_channels, sizeof(int16_t));
    far = (int16_t *)calloc(frame_size * config.out_channels, sizeof(int16_t));
    out = (int16_t *)calloc(frame_size * config.rec_channels, sizeof(int16_t));

    if (rec == NULL || far == NULL || out == NULL)
    {
        printf("Fail to allocate memory\n");
        exit(1);
    }

    // Configures signal handling.
    struct sigaction sig_int_handler;
    sig_int_handler.sa_handler = int_handler;
    sigemptyset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;
    sigaction(SIGINT, &sig_int_handler, NULL);

    echo_state = speex_echo_state_init_mc(frame_size,
                                          config.filter_length,
                                          config.rec_channels,
                                          config.out_channels);
    speex_echo_ctl(echo_state, SPEEX_ECHO_SET_SAMPLING_RATE, &(config.rate));

    audio_start(&config);

    fifo_setup(&g_ringbuffer[PROCESSED_INDEX]);

    printf("Running... Press Ctrl+C to exit\n");

    int wait_us = frame_size * 1000000 / config.rate / 2;

    // system delay between recording and playback
    while (PaUtil_GetRingBufferReadAvailable(&g_ringbuffer[CAPTURE_INDEX]) < delay)
    {
        usleep(wait_us);
    }
    PaUtil_AdvanceRingBufferReadIndex(&g_ringbuffer[CAPTURE_INDEX], delay);

    while (!g_is_quit)
    {
        while (!g_is_quit && PaUtil_GetRingBufferReadAvailable(&g_ringbuffer[CAPTURE_INDEX]) < frame_size)
        {
            usleep(wait_us);
        }
        PaUtil_ReadRingBuffer(&g_ringbuffer[CAPTURE_INDEX], rec, frame_size);

        while (!g_is_quit && PaUtil_GetRingBufferReadAvailable(&g_ringbuffer[PLAYED_INDEX]) < frame_size)
        {
            usleep(wait_us);
        }
        PaUtil_ReadRingBuffer(&g_ringbuffer[PLAYED_INDEX], far, frame_size);

        if (!config.bypass)
        {
            speex_echo_cancellation(echo_state, rec, far, out);
        }
        else
        {
            memcpy(out, rec, frame_size * config.rec_channels * config.bits_per_sample / 8);
        }

        if (fp_far)
        {
            fwrite(rec, 2, frame_size * config.rec_channels, fp_rec);
            fwrite(far, 2, frame_size, fp_far);
            fwrite(out, 2, frame_size * config.rec_channels, fp_out);
        }

        PaUtil_WriteRingBuffer(&g_ringbuffer[PROCESSED_INDEX], out, frame_size);
    }

    if (fp_far)
    {
        fclose(fp_rec);
        fclose(fp_far);
        fclose(fp_out);
    }

    free(rec);
    free(far);
    free(out);

    audio_stop();

    exit(0);

    return 0;
}
