// protocore-conformance-isolate -- protoCore's own reference one-case-per-process
// runner, built against SelfHost.
//
// Each embedder builds its OWN isolate binary from its own Host with
// PROTOCORE_CONFORMANCE_ISOLATE_MAIN(HostType), because the binary must link the
// embedder's adaptor.  This one exists so that the aborting case
// (heap.ceiling_progress, whose failure mode is std::abort inside
// waitForHeapHeadroom) can be observed from outside by exit status against a
// host protoCore controls.
#include "SelfHost.h"

PROTOCORE_CONFORMANCE_ISOLATE_MAIN(proto::conformance::SelfHost)
