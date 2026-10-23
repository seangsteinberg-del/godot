/**************************************************************************/
/*  movie_writer.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/io/image.h"
#include "core/templates/local_vector.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_server.h"

class MovieWriter : public Object {
	GDCLASS(MovieWriter, Object);

	uint64_t fps = 0;
	uint64_t mix_rate = 0;
	uint32_t audio_channels = 0;

	// The output resolution, which can differ from the window size.
	// Used as a base for resizing all subsequent frames if their resolution differs.
	Vector2i movie_size;

	float cpu_time = 0.0f;
	float gpu_time = 0.0f;
	uint64_t encoding_time_usec = 0;
	static uint64_t frame_usec; // THE WAITS NAMED (Longshot patch #51): the writer's share of the main thread's frame, cumulative microseconds (the audio mixed, the room waited, the read asked)

	String project_name;

	LocalVector<int32_t> audio_mix_buffer;

	// THE TAKE WITHOUT THE WAIT (longshot #38, 10-19): the frame is read back asynchronously (RS::texture_2d_get_async - the bytes
	// come back two frames on, the CPU never stalls on the GPU) and encoded and written by a worker thread in serial order, so a
	// take's wall frame is the longer of the game's and the device's, not their sum with the encode's (the 10-19 16:36 take: a wall
	// frame of 145 ms where the device took 56 and the encode 27). The frames are the same frames. A script's writer (_write_frame
	// overridden) keeps the synchronous road: its method runs on the main thread.
	struct PendingFrame {
		uint64_t serial = 0;
		Ref<Image> image;
		LocalVector<int32_t> audio;
		bool hdr_2d = false;
	};
	enum {
		FRAMES_IN_FLIGHT_MAX = 8 // frames asked of the device and not yet written, at most (the memory bound: eight 1080p frames, 64 MB). It must
								 // exceed the device's frames in flight (frame_queue_size, 2-3): a frame's bytes come back only in a LATER frame's
								 // _begin_frame, which cannot run while add_frame waits for room - so the room is the readback's latency plus the
								 // encoder's lag; past it the take waits for the encoder, never for the device.
	};
	bool async_frames = false;
	uint64_t frames_requested = 0;
	uint64_t frames_written = 0; // the worker's; read by end() after the join
	RID main_vp_texture_rid;
	Mutex frames_mutex;
	Semaphore frames_ready;
	Semaphore frames_room;
	List<PendingFrame> frames_queue; // read-back frames in serial order
	HashMap<uint64_t, PendingFrame> frames_pending; // their audio and flags, waiting for the image
	Thread worker;
	bool worker_stop = false;
	SafeNumeric<uint64_t> worker_encoding_usec;
	void _frame_prepare(PendingFrame &p_frame);
	void _frame_queue(const PendingFrame &p_frame); // (the mutex held by the caller)
	void _frame_read(const Ref<Image> &p_image, uint64_t p_serial);
	static void _worker_main(void *p_self);

	enum {
		MAX_WRITERS = 8
	};
	static MovieWriter *writers[];
	static uint32_t writer_count;

protected:
	virtual uint32_t get_audio_mix_rate() const;
	virtual AudioServer::SpeakerMode get_audio_speaker_mode() const;

	virtual Error write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path);
	virtual Error write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data);
	virtual void write_end();

	GDVIRTUAL0RC_REQUIRED(uint32_t, _get_audio_mix_rate)
	GDVIRTUAL0RC_REQUIRED(AudioServer::SpeakerMode, _get_audio_speaker_mode)

	GDVIRTUAL1RC_REQUIRED(bool, _handles_file, const String &)
	GDVIRTUAL0RC_REQUIRED(Vector<String>, _get_supported_extensions)

	GDVIRTUAL3R_REQUIRED(Error, _write_begin, const Size2i &, uint32_t, const String &)
	GDVIRTUAL2R_REQUIRED(Error, _write_frame, const Ref<Image> &, GDExtensionConstPtr<int32_t>)
	GDVIRTUAL0_REQUIRED(_write_end)

	static void _bind_methods();

public:
	virtual bool handles_file(const String &p_path) const;
	virtual void get_supported_extensions(List<String> *r_extensions) const;

	static void add_writer(MovieWriter *p_writer);
	static uint64_t get_frame_usec(); // THE WAITS NAMED (Longshot patch #51)
	static MovieWriter *find_writer_for_file(const String &p_file);

	void begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path);
	void add_frame();
	void finish_frames(); // THE TAKE'S LAST FRAMES (longshot #38): the pending readbacks forced while the viewport still stands - the main loop's last iteration calls it

	static void set_extensions_hint();

	void end();
};
