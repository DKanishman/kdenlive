#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2021 Jean-Baptiste Mardelle <jb@kdenlive.org>
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pip3 install vosk
#pip3 install srt

from vosk import Model, KaldiRecognizer, SetLogLevel
import sys
import os
import subprocess
import argparse
import json
import wave

SetLogLevel(-1)


def main():

    parser = argparse.ArgumentParser("VOSK to text script")
    parser.add_argument("-S", "--src", help="source audio file")
    parser.add_argument("-M", "--model", help="model name")
    parser.add_argument("-D", "--model_directory", help="the folder where the model is")
    parser.add_argument("-I", "--in_point", help="in point if not starting from 0", default="0")
    parser.add_argument("-O", "--out_point", help="out point if not operating on full file", default="0")
    parser.add_argument("-F", "--ffmpeg_path", help="path for ffmpeg", default="ffmpeg")
    parser.add_argument("--pcm_source", action="store_true", help="read pre-extracted mono 16k PCM WAV without ffmpeg")
    parser.add_argument("--result_file", help="write exact native recognizer results as UTF-8 JSON")
    args = parser.parse_args()

    src = args.src

    if src is None:
        config = vars(args)
        print(config)
        sys.exit()

    source = src.replace('"', '')
    print(f"ANALYSING SOURCE FILE: {source}.")
    if not os.path.exists(source):
        print(f"Source file does not exist: {source}.")
        return 1

    model = args.model
    ffmpeg_path = args.ffmpeg_path

    os.chdir(args.model_directory)

    if not os.path.exists(model):
        print(f"Please download the model from https://alphacephei.com/vosk/models and unpack as {model} in the current folder.")
        exit (1)

    sample_rate=16000
    voskModel = Model(model)
    rec = KaldiRecognizer(voskModel, sample_rate)
    rec.SetWords(True)

    # The GUI retains its FFmpeg input. MCP pre-extracts the exact source range
    # and reads PCM directly, avoiding a child process that could outlive abort.
    process = None
    pcm = None
    if args.pcm_source:
        if float(args.in_point) != 0 or float(args.out_point) != 0:
            raise ValueError("PCM source ranges must be extracted before recognition")
        pcm = wave.open(source, "rb")
        if (pcm.getnchannels(), pcm.getsampwidth(), pcm.getframerate(), pcm.getcomptype()) != (1, 2, sample_rate, "NONE"):
            pcm.close()
            raise ValueError("PCM input must be uncompressed mono 16-bit 16000 Hz WAV")
        if pcm.getnframes() > sample_rate * 1800:
            pcm.close()
            raise ValueError("PCM input exceeds the 30-minute job limit")
        read_audio = lambda: pcm.readframes(2000)
    else:
        command = [ffmpeg_path, '-loglevel', 'quiet', '-i', source]
        if float(args.in_point) > 0 or float(args.out_point) > 0:
            command += ['-ss', args.in_point, '-t', args.out_point]
        command += ['-ar', str(sample_rate), '-ac', '1', '-f', 's16le', '-']
        process = subprocess.Popen(command, stdout=subprocess.PIPE)
        read_audio = lambda: process.stdout.read(4000)

    results = []
    try:
        while True:
            data = read_audio()
            if not data:
                result = rec.FinalResult()
            elif rec.AcceptWaveform(data):
                result = rec.Result()
            else:
                continue
            if args.result_file:
                results.append(json.loads(result))
            else:
                sys.stdout.buffer.write(result.encode('utf-8'))
                sys.stdout.flush()
            if not data:
                break
    finally:
        if pcm is not None:
            pcm.close()
        if process is not None:
            process.stdout.close()
            if process.wait() != 0:
                raise RuntimeError("FFmpeg audio extraction failed")

    if args.result_file:
        with open(args.result_file, "w", encoding="utf-8") as output:
            json.dump({"results": results}, output, ensure_ascii=False, allow_nan=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
