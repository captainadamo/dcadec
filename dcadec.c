/*
 * This file is part of dcadec.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif

#include "libdcadec/dca_stream.h"
#include "libdcadec/dca_context.h"
#include "libdcadec/dca_waveout.h"

static void print_help(char *name)
{
    fprintf(stderr,
"Usage: %s [-bcfhPqsx] <input.dts> [output.wav]\n"
"dcadec is a free DTS Coherent Acoustics decoder. Supported options:\n"
"\n"
"-b\n"
"    Force fixed point DTS core interpolation. Developer option, degrades sound\n"
"    quality.\n"
"\n"
"-c\n"
"    Force decoding of DTS core only without extensions.\n"
"\n"
"-f\n"
"    Use FIR filter for floating point DTS core LFE channel interpolation.\n"
"\n"
"-h\n"
"    Show this help message.\n"
"\n"
"-P\n"
"    Disable progress indicator.\n"
"\n"
"-q\n"
"    Be quiet. Disables informational messages and progress indicator. Warnings\n"
"    and errors are still printed.\n"
"\n"
"-s\n"
"    Force bit width reduction of DTS core from 24 bit to source PCM resolution.\n"
"    Developer option, degrades sound quality.\n"
"\n"
"-x\n"
"    Force use of X96 synthesis filter for DTS core interpolation. Developer\n"
"    option, degrades sound quality.\n"
"\n"
"When run without output file name argument, prints information about DTS file\n"
"to stdout and exits.\n"
"\n"
"Single dash in place of input or output file name argument means to read from\n"
"stdin or write to stdout, respectively.\n"
"\n"
"dcadec comes with ABSOLUTELY NO WARRANTY. This is free software, and you are\n"
"welcome to redistribute it under certain conditions; see GNU General Public\n"
"License version 2 for details.\n", name);
}

static void print_info(struct dcadec_context *context)
{
    struct dcadec_core_info *core = dcadec_context_get_core_info(context);
    if (core) {
        fprintf(stderr, "DTS Core Audio: %d.%d ch, %d Hz, %d bit",
            core->nchannels, !!core->lfe_present, core->sample_rate,
            core->source_pcm_res);
        if (core->es_format)
            fprintf(stderr, ", ES");
        if (core->bit_rate > 0)
            fprintf(stderr, ", %.f kbps", core->bit_rate / 1000.0f);
        fprintf(stderr, "\n");
        dcadec_context_free_core_info(core);
    }

    struct dcadec_exss_info *exss = dcadec_context_get_exss_info(context);
    if (exss) {
        if (exss->profile & DCADEC_PROFILE_HD_MA)
            fprintf(stderr, "DTS-HD Master Audio");
        else if (exss->profile & DCADEC_PROFILE_HD_HRA)
            fprintf(stderr, "DTS-HD High-Resolution Audio");
        else
            fprintf(stderr, "DTS-HD Unknown Profile");
        fprintf(stderr, ": %d ch, %d Hz, %d bit\n",
            exss->nchannels, exss->sample_rate, exss->bits_per_sample);
        dcadec_context_free_exss_info(exss);
    }
}

static bool interrupted;

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType)
{
    (void)dwCtrlType;
    interrupted = true;
    return TRUE;
}
#else
static void signal_handler(int sig)
{
    (void)sig;
    interrupted = true;
}
#endif

int main(int argc, char **argv)
{
    int flags = 0;
    bool no_progress = false;
    bool quiet = false;

    int opt;
    while ((opt = getopt(argc, argv, "bcfhPqsx")) != -1) {
        switch (opt) {
        case 'b':
            flags |= DCADEC_FLAG_CORE_BIT_EXACT;
            break;
        case 'c':
            flags |= DCADEC_FLAG_CORE_ONLY;
            break;
        case 'f':
            flags |= DCADEC_FLAG_CORE_LFE_FIR;
            break;
        case 'h':
            print_help(argv[0]);
            return 0;
        case 'P':
            no_progress = true;
            break;
        case 'q':
            quiet = true;
            break;
        case 's':
            flags |= DCADEC_FLAG_CORE_SOURCE_PCM_RES;
            break;
        case 'x':
            flags |= DCADEC_FLAG_CORE_SYNTH_X96;
            break;
        default:
            print_help(argv[0]);
            return 1;
        }
    }

    no_progress |= quiet;

    if (optind >= argc) {
        print_help(argv[0]);
        return 1;
    }

    char *fn = argv[optind];
    struct dcadec_stream *stream = dcadec_stream_open(strcmp(fn, "-") ? fn : NULL);
    if (!stream) {
        fprintf(stderr, "Couldn't open input file\n");
        return 1;
    }

    uint8_t *packet;
    size_t size;
    int ret;

    if ((ret = dcadec_stream_read(stream, &packet, &size)) < 0) {
        fprintf(stderr, "Error %d reading packet\n", ret);
        dcadec_stream_close(stream);
        return 1;
    }

    if (ret == 0) {
        fprintf(stderr, "This doesn't look like a 16-bit DTS bit stream\n");
        dcadec_stream_close(stream);
        return 1;
    }

    struct dcadec_context *context = dcadec_context_create(flags);
    if (!context) {
        fprintf(stderr, "Couldn't create decoder context\n");
        dcadec_stream_close(stream);
        return 1;
    }

    if ((ret = dcadec_context_parse(context, packet, size)) < 0) {
        fprintf(stderr, "Error %d parsing packet\n", ret);
        dcadec_context_destroy(context);
        dcadec_stream_close(stream);
        return 1;
    }

    if (!quiet)
        print_info(context);

    if (optind + 1 >= argc) {
        dcadec_context_destroy(context);
        dcadec_stream_close(stream);
        return 0;
    }

    fn = argv[optind + 1];
    struct dcadec_waveout *waveout = dcadec_waveout_open(strcmp(fn, "-") ? fn : NULL);
    if (!waveout) {
        fprintf(stderr, "Couldn't open output file\n");
        dcadec_context_destroy(context);
        dcadec_stream_close(stream);
        return 1;
    }

    int last_progress = -1;

#ifdef _WIN32
    SetConsoleCtrlHandler(&console_ctrl_handler, TRUE);
#else
    signal(SIGINT, &signal_handler);
#endif

    if (!quiet) {
        if (flags & DCADEC_FLAG_CORE_ONLY)
            fprintf(stderr, "Decoding (core only)...\n");
        else
            fprintf(stderr, "Decoding...\n");
    }

    while (!interrupted) {
        int **samples, nsamples, channel_mask, sample_rate, bits_per_sample;
        if ((ret = dcadec_context_filter(context, &samples, &nsamples,
                                         &channel_mask, &sample_rate,
                                         &bits_per_sample, NULL)) < 0) {
            fprintf(stderr, "Error %d filtering frame\n", ret);
            break;
        }

        if ((ret = dcadec_waveout_write(waveout, samples, nsamples,
                                        channel_mask, sample_rate,
                                        bits_per_sample)) < 0) {
            fprintf(stderr, "Error %d writing PCM output\n", ret);
            break;
        }

        if ((ret = dcadec_stream_read(stream, &packet, &size)) < 0) {
            fprintf(stderr, "Error %d reading packet\n", ret);
            break;
        }

        if (!no_progress) {
            int progress = dcadec_stream_progress(stream);
            if (progress != last_progress) {
                fprintf(stderr, "Progress: %d%%\r", progress);
                last_progress = progress;
            }
        }

        if (ret == 0)
            break;

        if ((ret = dcadec_context_parse(context, packet, size)) < 0) {
            fprintf(stderr, "Error %d parsing packet\n", ret);
            break;
        }
    }

    if (!quiet) {
        if (last_progress != -1)
            fprintf(stderr, "\n");
        if (interrupted)
            fprintf(stderr, "Interrupted.\n");
        else if (ret == 0)
            fprintf(stderr, "Completed.\n");
    }

    dcadec_waveout_close(waveout);
    dcadec_context_destroy(context);
    dcadec_stream_close(stream);
    return !!ret;
}
