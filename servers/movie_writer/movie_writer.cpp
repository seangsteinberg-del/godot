/**************************************************************************/
/*  movie_writer.cpp                                                      */
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

#include "movie_writer.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/time.h"
#include "core/templates/rb_set.h"
#include "scene/main/window.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/display/display_server_enums.h"
#include "servers/rendering/rendering_server.h"

MovieWriter *MovieWriter::writers[MovieWriter::MAX_WRITERS];
uint32_t MovieWriter::writer_count = 0;
uint64_t MovieWriter::frame_usec = 0; // THE WAITS NAMED (Longshot patch #51)

uint64_t MovieWriter::get_frame_usec() {
	return frame_usec;
}

void MovieWriter::add_writer(MovieWriter *p_writer) {
	ERR_FAIL_COND(writer_count == MAX_WRITERS);
	writers[writer_count++] = p_writer;
}

MovieWriter *MovieWriter::find_writer_for_file(const String &p_file) {
	for (int32_t i = writer_count - 1; i >= 0; i--) { // More recent last, to have override ability.
		if (writers[i]->handles_file(p_file)) {
			return writers[i];
		}
	}
	return nullptr;
}

uint32_t MovieWriter::get_audio_mix_rate() const {
	uint32_t ret = 48000;
	GDVIRTUAL_CALL(_get_audio_mix_rate, ret);
	return ret;
}
AudioServer::SpeakerMode MovieWriter::get_audio_speaker_mode() const {
	AudioServer::SpeakerMode ret = AudioServer::SPEAKER_MODE_STEREO;
	GDVIRTUAL_CALL(_get_audio_speaker_mode, ret);
	return ret;
}

Error MovieWriter::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	Error ret = ERR_UNCONFIGURED;
	GDVIRTUAL_CALL(_write_begin, p_movie_size, p_fps, p_base_path, ret);
	return ret;
}

Error MovieWriter::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	Error ret = ERR_UNCONFIGURED;
	GDVIRTUAL_CALL(_write_frame, p_image, p_audio_data, ret);
	return ret;
}

void MovieWriter::write_end() {
	GDVIRTUAL_CALL(_write_end);
}

bool MovieWriter::handles_file(const String &p_path) const {
	bool ret = false;
	GDVIRTUAL_CALL(_handles_file, p_path, ret);
	return ret;
}

void MovieWriter::get_supported_extensions(List<String> *r_extensions) const {
	Vector<String> exts;
	GDVIRTUAL_CALL(_get_supported_extensions, exts);
	for (int i = 0; i < exts.size(); i++) {
		r_extensions->push_back(exts[i]);
	}
}

void MovieWriter::begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	project_name = GLOBAL_GET("application/config/name");
	movie_size = p_movie_size;

	print_line(vformat(U"Movie Maker mode enabled, recording movie in %s×%s @ %d FPS...", movie_size.width, movie_size.height, p_fps));

	// Check for available disk space and warn the user if needed.
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	String path = p_base_path.get_base_dir();
	if (path.is_relative_path()) {
		path = "res://" + path;
	}
	dir->open(path);
	if (dir->get_space_left() < 10 * Math::pow(1024.0, 3.0)) {
		// Less than 10 GiB available.
		WARN_PRINT(vformat("Current available space on disk is low (%s). MovieWriter will fail during movie recording if the disk runs out of available space.", String::humanize_size(dir->get_space_left())));
	}

	cpu_time = 0.0f;
	gpu_time = 0.0f;
	encoding_time_usec = 0;

	mix_rate = get_audio_mix_rate();
	AudioDriverDummy::get_dummy_singleton()->set_mix_rate(mix_rate);
	AudioDriverDummy::get_dummy_singleton()->set_speaker_mode(AudioDriver::SpeakerMode(get_audio_speaker_mode()));
	fps = p_fps;
	if ((mix_rate % fps) != 0) {
		WARN_PRINT("MovieWriter's audio mix rate (" + itos(mix_rate) + ") can not be divided by the recording FPS (" + itos(fps) + "). Audio may go out of sync over time.");
	}

	audio_channels = AudioDriverDummy::get_dummy_singleton()->get_channels();
	audio_mix_buffer.resize(mix_rate * audio_channels / fps);

	write_begin(movie_size, p_fps, p_base_path);

	// THE TAKE WITHOUT THE WAIT (longshot #38): the worker that encodes and writes, unless a script writes the frames
	async_frames = !GDVIRTUAL_IS_OVERRIDDEN(_write_frame);
	frames_requested = 0;
	frames_written = 0;
	worker_stop = false;
	worker_encoding_usec.set(0);
	if (async_frames) {
		for (int i = 0; i < FRAMES_IN_FLIGHT_MAX; i++) {
			frames_room.post();
		}
		worker.start(_worker_main, this);
	}
}

void MovieWriter::_bind_methods() {
	ClassDB::bind_static_method("MovieWriter", D_METHOD("add_writer", "writer"), &MovieWriter::add_writer);
	ClassDB::bind_static_method("MovieWriter", D_METHOD("get_frame_usec"), &MovieWriter::get_frame_usec); // THE WAITS NAMED (Longshot patch #51)

	GDVIRTUAL_BIND(_get_audio_mix_rate)
	GDVIRTUAL_BIND(_get_audio_speaker_mode)

	GDVIRTUAL_BIND(_handles_file, "path")
	GDVIRTUAL_BIND(_get_supported_extensions)

	GDVIRTUAL_BIND(_write_begin, "movie_size", "fps", "base_path")
	GDVIRTUAL_BIND(_write_frame, "frame_image", "audio_frame_block")
	GDVIRTUAL_BIND(_write_end)

	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/mix_rate", PROPERTY_HINT_RANGE, "8000,192000,1,suffix:Hz"), 48000);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/speaker_mode", PROPERTY_HINT_ENUM, "Stereo,3.1,5.1,7.1"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "editor/movie_writer/video_quality", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), 0.75);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/audio_bit_depth", PROPERTY_HINT_ENUM, "16:16,32:32"), 16);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "editor/movie_writer/ogv/audio_quality", PROPERTY_HINT_RANGE, "-0.1,1.0,0.01"), 0.5);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ogv/encoding_speed", PROPERTY_HINT_ENUM, "Fastest (Lowest Efficiency):4,Fast (Low Efficiency):3,Slow (High Efficiency):2,Slowest (Highest Efficiency):1"), 4);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ogv/keyframe_interval", PROPERTY_HINT_RANGE, "1,1024,1"), 64);

	// Used by the editor.
	GLOBAL_DEF_BASIC("editor/movie_writer/movie_file", "");
	GLOBAL_DEF_BASIC("editor/movie_writer/disable_vsync", false);
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::INT, "editor/movie_writer/fps", PROPERTY_HINT_RANGE, "1,300,1,suffix:FPS"), 60);
}

void MovieWriter::set_extensions_hint() {
	RBSet<String> found;
	for (uint32_t i = 0; i < writer_count; i++) {
		List<String> extensions;
		writers[i]->get_supported_extensions(&extensions);
		for (const String &ext : extensions) {
			found.insert(ext);
		}
	}

	String ext_hint;

	for (const String &S : found) {
		if (ext_hint != "") {
			ext_hint += ",";
		}
		ext_hint += "*." + S;
	}
	ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::STRING, "editor/movie_writer/movie_file", PROPERTY_HINT_GLOBAL_SAVE_FILE, ext_hint));
}

void MovieWriter::add_frame() {
	const uint64_t frame_t0 = Time::get_singleton()->get_ticks_usec(); // THE WAITS NAMED (Longshot patch #51): the writer's share of the frame
	const int movie_time_seconds = Engine::get_singleton()->get_frames_drawn() / fps;
	const int frame_remainder = Engine::get_singleton()->get_frames_drawn() % fps;
	const String movie_time = vformat("%s:%s:%s:%s",
			String::num(movie_time_seconds / 3600, 0).pad_zeros(2),
			String::num((movie_time_seconds % 3600) / 60, 0).pad_zeros(2),
			String::num(movie_time_seconds % 60, 0).pad_zeros(2),
			String::num(frame_remainder, 0).pad_zeros(2));

	Window *main_window = Window::get_from_id(DisplayServerEnums::MAIN_WINDOW_ID);
	if (main_window) {
		main_window->set_title(vformat("MovieWriter: Frame %d (time: %s) - %s", Engine::get_singleton()->get_frames_drawn(), movie_time, project_name));
	}

	RID main_vp_rid = RenderingServer::get_singleton()->viewport_find_from_screen_attachment(DisplayServerEnums::MAIN_WINDOW_ID);
	RID main_vp_texture = RenderingServer::get_singleton()->viewport_get_texture(main_vp_rid);
	main_vp_texture_rid = main_vp_texture;
	const bool hdr_2d = RenderingServer::get_singleton()->viewport_is_using_hdr_2d(main_vp_rid);

	RenderingServer::get_singleton()->viewport_set_measure_render_time(main_vp_rid, true);
	cpu_time += RenderingServer::get_singleton()->viewport_get_measured_render_time_cpu(main_vp_rid);
	cpu_time += RenderingServer::get_singleton()->get_frame_setup_time_cpu();
	gpu_time += RenderingServer::get_singleton()->viewport_get_measured_render_time_gpu(main_vp_rid);

	AudioDriverDummy::get_dummy_singleton()->mix_audio(mix_rate / fps, audio_mix_buffer.ptr());

	if (!async_frames) {
		// a script's writer: the frame read at once and handed over on this thread
		PendingFrame fr;
		fr.image = RenderingServer::get_singleton()->texture_2d_get(main_vp_texture);
		fr.hdr_2d = hdr_2d;
		ERR_FAIL_COND_MSG(fr.image.is_null(), "The take's frame could not be read from the device.");
		_frame_prepare(fr);
		uint64_t encoding_start_usec = Time::get_singleton()->get_ticks_usec();
		write_frame(fr.image, audio_mix_buffer.ptr());
		uint64_t encoding_end_usec = Time::get_singleton()->get_ticks_usec();
		encoding_time_usec += encoding_end_usec - encoding_start_usec;
		frame_usec += Time::get_singleton()->get_ticks_usec() - frame_t0;
		return;
	}

	// THE TAKE WITHOUT THE WAIT (longshot #38): this frame's audio kept under its serial, the device asked for its pixels (the copy
	// rides this frame's command buffer, after its drawing), the worker writes it in serial order once the bytes are back
	frames_room.wait();
	const uint64_t serial = frames_requested++;
	{
		MutexLock lock(frames_mutex);
		PendingFrame &fr = frames_pending[serial];
		fr.serial = serial;
		fr.hdr_2d = hdr_2d;
		fr.audio = audio_mix_buffer;
	}
	RenderingServer::get_singleton()->texture_2d_get_async(main_vp_texture, callable_mp(this, &MovieWriter::_frame_read).bind(serial));
	frame_usec += Time::get_singleton()->get_ticks_usec() - frame_t0;
}

// the synchronous road's steps on a read-back frame: the crop and resize to the movie's size, the HDR 2D conversion
void MovieWriter::_frame_prepare(PendingFrame &p_frame) {
	Ref<Image> vp_tex = p_frame.image;
	if (vp_tex->get_size() != movie_size) {
		// Resize the texture to the output resolution if it differs from the current viewport size.
		// This ensures all frames have the same resolution, as not all video formats and players
		// support resolution changes during playback.

		const float src_aspect = vp_tex->get_size().aspect();
		const float dst_aspect = movie_size.aspect();

		int crop_width = vp_tex->get_size().width;
		int crop_height = vp_tex->get_size().height;
		int crop_x = 0;
		int crop_y = 0;

		// If the aspect ratio differs, crop the image to cover the base resolution's aspect ratio
		// in a way similar to `TextureRect.STRETCH_KEEP_ASPECT_COVERED`.
		if (src_aspect > dst_aspect) {
			// Source is wider, crop horizontally.
			crop_width = int(vp_tex->get_size().height * dst_aspect);
			crop_x = (vp_tex->get_size().width - crop_width) / 2;
			vp_tex->crop_from_point(crop_x, crop_y, crop_width, crop_height);
		} else if (src_aspect < dst_aspect) {
			// Source is taller, crop vertically.
			crop_height = int(vp_tex->get_size().width / dst_aspect);
			crop_y = (vp_tex->get_size().height - crop_height) / 2;
			vp_tex->crop_from_point(crop_x, crop_y, crop_width, crop_height);
		}

		vp_tex->resize(movie_size.width, movie_size.height, Image::INTERPOLATE_BILINEAR);
	}

	if (p_frame.hdr_2d) {
		vp_tex->convert(Image::FORMAT_RGBA8);
		vp_tex->linear_to_srgb();
	}
}

void MovieWriter::_frame_queue(const PendingFrame &p_frame) {
	List<PendingFrame>::Element *e = frames_queue.back();
	while (e && e->get().serial > p_frame.serial) {
		e = e->prev();
	}
	if (e) {
		frames_queue.insert_after(e, p_frame);
	} else {
		frames_queue.push_front(p_frame);
	}
}

// the device's answer (on the rendering thread, in a later frame's _begin_frame): the frame joins the queue in serial order
void MovieWriter::_frame_read(const Ref<Image> &p_image, uint64_t p_serial) {
	MutexLock lock(frames_mutex);
	HashMap<uint64_t, PendingFrame>::Iterator it = frames_pending.find(p_serial);
	ERR_FAIL_COND_MSG(!it, vformat("The take's frame %d came back from the device unasked.", p_serial));
	PendingFrame fr = it->value;
	frames_pending.remove(it);
	fr.image = p_image; // (a null image is dropped by the worker, and said)
	_frame_queue(fr);
	frames_ready.post();
}

// the worker: every frame whose serial is the next one, prepared, encoded and written; it stops once told and drained
void MovieWriter::_worker_main(void *p_self) {
	MovieWriter *w = (MovieWriter *)p_self;
	while (true) {
		w->frames_ready.wait();
		while (true) {
			PendingFrame fr;
			{
				MutexLock lock(w->frames_mutex);
				if (w->frames_queue.is_empty() || w->frames_queue.front()->get().serial != w->frames_written) {
					break; // the next frame in order is not back yet
				}
				fr = w->frames_queue.front()->get();
				w->frames_queue.pop_front();
			}
			if (fr.image.is_valid()) {
				uint64_t t0 = Time::get_singleton()->get_ticks_usec();
				w->_frame_prepare(fr);
				Error err = w->write_frame(fr.image, fr.audio.ptr());
				w->worker_encoding_usec.add(Time::get_singleton()->get_ticks_usec() - t0);
				if (err != OK) {
					ERR_PRINT(vformat("The take's frame %d could not be written (error %d).", fr.serial, err));
				}
			} else {
				ERR_PRINT(vformat("The take's frame %d had no image: dropped.", fr.serial));
			}
			w->frames_written++;
			w->frames_room.post();
		}
		bool done = false;
		{
			MutexLock lock(w->frames_mutex);
			done = w->worker_stop && w->frames_queue.is_empty();
		}
		if (done) {
			break;
		}
	}
}

// THE TAKE'S LAST FRAMES (longshot #38): called by the main loop's last iteration, after its add_frame and before the scene tree is
// finalized. A synchronous read of the viewport's texture stalls every submitted frame and fires its pending readbacks, then submits the
// frame being recorded - whose own readback (the last add_frame's) fires only on the next stall of its slot - so the read is repeated
// until nothing is pending (two reads in practice). After the tree is gone the texture is gone, and end() can only report.
void MovieWriter::finish_frames() {
	if (!async_frames) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		bool pending = false;
		{
			MutexLock lock(frames_mutex);
			pending = !frames_pending.is_empty();
		}
		if (!pending) {
			return;
		}
		Ref<Image> last = RenderingServer::get_singleton()->texture_2d_get(main_vp_texture_rid);
	}
}

void MovieWriter::end() {
	uint64_t encoding_start_usec = Time::get_singleton()->get_ticks_usec();
	if (async_frames) {
		// THE LAST FRAMES (longshot #38): finish_frames() has forced the pending readbacks in the main loop's last iteration, while the
		// viewport stood (here the scene tree is finalized already: its texture is gone, and a read of it cannot stall the device). A
		// frame the device never returned is dropped and said; the worker drains the queue in order and stops
		{
			MutexLock lock(frames_mutex);
			for (const KeyValue<uint64_t, PendingFrame> &kv : frames_pending) {
				ERR_PRINT(vformat("The take's frame %d never came back from the device: dropped.", kv.key));
				_frame_queue(kv.value);
			}
			frames_pending.clear();
			worker_stop = true;
		}
		frames_ready.post();
		worker.wait_to_finish();
		encoding_time_usec += worker_encoding_usec.get();
		if (frames_written != frames_requested) {
			ERR_PRINT(vformat("The take asked the device for %d frames and wrote %d.", frames_requested, frames_written));
		}
	}
	write_end();
	uint64_t encoding_end_usec = Time::get_singleton()->get_ticks_usec();
	encoding_time_usec += encoding_end_usec - encoding_start_usec;

	// Print a report with various statistics.
	print_line("--------------------------------------------------------------------------------");
	String movie_path = Engine::get_singleton()->get_write_movie_path();
	if (movie_path.is_relative_path()) {
		// Print absolute path to make finding the file easier,
		// and to make it clickable in terminal emulators that support this.
		movie_path = ProjectSettings::get_singleton()->globalize_path("res://").path_join(movie_path);
	}
	print_line(vformat("Done recording movie at path: %s", movie_path));

	const int movie_time_seconds = Engine::get_singleton()->get_frames_drawn() / fps;
	const int frame_remainder = Engine::get_singleton()->get_frames_drawn() % fps;
	const String movie_time = vformat("%s:%s:%s:%s",
			String::num(movie_time_seconds / 3600, 0).pad_zeros(2),
			String::num((movie_time_seconds % 3600) / 60, 0).pad_zeros(2),
			String::num(movie_time_seconds % 60, 0).pad_zeros(2),
			String::num(frame_remainder, 0).pad_zeros(2));

	const int real_time_seconds = Time::get_singleton()->get_ticks_msec() / 1000;
	const String real_time = vformat("%s:%s:%s",
			String::num(real_time_seconds / 3600, 0).pad_zeros(2),
			String::num((real_time_seconds % 3600) / 60, 0).pad_zeros(2),
			String::num(real_time_seconds % 60, 0).pad_zeros(2));

	print_line(vformat("%d frames at %d FPS (movie length: %s), recorded in %s (%d%% of real-time speed).", Engine::get_singleton()->get_frames_drawn(), fps, movie_time, real_time, (float(MAX(1, movie_time_seconds)) / MAX(1, real_time_seconds)) * 100));
	print_line(vformat("CPU render time: %.2f seconds (average: %.2f ms/frame)", cpu_time / 1000, cpu_time / Engine::get_singleton()->get_frames_drawn()));
	print_line(vformat("GPU render time: %.2f seconds (average: %.2f ms/frame)", gpu_time / 1000, gpu_time / Engine::get_singleton()->get_frames_drawn()));
	print_line(vformat("Encoding time: %.2f seconds (average: %.2f ms/frame)", encoding_time_usec / 1000000.f, encoding_time_usec / 1000.f / Engine::get_singleton()->get_frames_drawn()));
	print_line("--------------------------------------------------------------------------------");
}
