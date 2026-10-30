/**************************************************************************/
/*  pipeline_compilation_ledger.h                                         */
/**************************************************************************/
/* THE HITCH NAMES ITS PIPELINES (longshot patch #88, 10-30, the codebase correction lane): the renderers' pipeline hash maps note
/* every pipeline they compile here - the shader's path or hint and the source (RenderingServerEnums::PipelineSource) - and the
/* frame ledger takes the list at a hitch (RenderingServer::get_pipeline_compilations_recent, which clears it). The last 64.    */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

class PipelineCompilationLedger {
	static Mutex mutex;
	static PackedStringArray recent;
	static const int RING = 64;

public:
	static void note(const String &p_name, int p_source);
	static PackedStringArray take();
};
