/**************************************************************************/
/*  pipeline_compilation_ledger.cpp                                       */
/**************************************************************************/
/* THE HITCH NAMES ITS PIPELINES (longshot patch #88): see the header.                                                            */
/**************************************************************************/

#include "pipeline_compilation_ledger.h"

Mutex PipelineCompilationLedger::mutex;
PackedStringArray PipelineCompilationLedger::recent;

void PipelineCompilationLedger::note(const String &p_name, int p_source) {
	MutexLock lock(mutex);
	if (recent.size() >= RING) {
		recent.remove_at(0);
	}
	recent.push_back((p_name.is_empty() ? String("(unnamed)") : p_name) + "|" + itos(p_source));
}

PackedStringArray PipelineCompilationLedger::take() {
	MutexLock lock(mutex);
	PackedStringArray out = recent;
	recent.clear();
	return out;
}
