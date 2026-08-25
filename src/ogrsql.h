#pragma once
#include "ogr.h"
#include <memory>
#include <string>

// OGRSQL (swq) subset over materialized layers: attribute filters
// (SetAttributeFilter semantics) and OGRSQL SELECT execution.

// parse + bind + apply `where` to lyr: features filtered in place,
// extent recomputed (dropped when the result is empty). On failure the
// reference's error lines are emitted and false returned; vector info
// also gets the trailing "SetAttributeFilter(...) failed." line (the
// filter verb reports the bare cause only).
bool ogrApplyAttributeFilter(OgrLayer &lyr, const std::string &where,
                             bool emitSetFilterFailed = true);

// execute an OGRSQL SELECT against the dataset's layers; on failure
// emits the reference's error lines and returns null (callers exit 1).
std::unique_ptr<OgrLayer> ogrExecuteSql(const OgrDataset &ds,
                                        const std::string &sql);
