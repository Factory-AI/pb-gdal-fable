#pragma once
#include "engine.h"
#include "ogr.h"
#include "spec.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// shared plumbing of the convert-delegating vector verbs (filter/select/
// sql and the geometry verbs): GDALG echo head, format resolution, input
// open and the delegate into vector_convert with a dataset-mutate hook

std::string vvJoinComma(const std::vector<std::string> &v);
std::string vvGq(const std::string &v);
std::string vvFmtReal(const std::string &raw);
std::string vvGdalgHead(ParseResult &r, const std::string &input,
                        bool hasInputLayer, bool commonOutputLayer);
int vvResolveVerbFormats(const CmdSpec &cmd, ParseResult &r,
                         std::string &driver);
int vvOpenInputDs(const CmdSpec &cmd, ParseResult &r,
                  const std::string &input,
                  std::unique_ptr<OgrDataset> &ds);
int vvOpenInputDsNoUsage(const CmdSpec &cmd, ParseResult &r,
                         const std::string &input,
                         std::unique_ptr<OgrDataset> &ds);
int vvDelegateVerb(ParseResult &r, const std::string &verb,
                   std::unique_ptr<OgrDataset> ds,
                   const std::string &gdalgCli, const std::string &driver,
                   bool forwardOutputLayer,
                   std::function<int(OgrDataset &)> mutate);
bool vvLayerSelected(const std::vector<std::string> &sel,
                     const std::string &active, const OgrLayer &l);
int vvVerbFormatArgCheck(const std::string &verb,
                         const std::string &argName, ParseResult &r);
