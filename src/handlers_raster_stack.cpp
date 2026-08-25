// gdal raster stack: shares the mosaic/stack machinery defined in
// handlers_raster_mosaic.cpp
#include "engine.h"

int rasterMosaicStackHandlerEntry(const CmdSpec &, ParseResult &);
std::string rasterMosaicStackArgValueCheckEntry(const std::string &,
                                                const std::string &);
int rasterMosaicStackArgCheckEntry(const std::string &, ParseResult &);
int rasterMosaicStackPreValidatorEntry(const CmdSpec &, ParseResult &);
bool rasterMosaicStackPostValidatorEntry(const CmdSpec &, ParseResult &,
                                         bool);

void registerRasterStackHandler()
{
    registerHandler("raster_stack", rasterMosaicStackHandlerEntry);
    registerArgValueCheck("raster_stack",
                          rasterMosaicStackArgValueCheckEntry);
    registerArgCheck("raster_stack", rasterMosaicStackArgCheckEntry);
    registerPreValidator("raster_stack",
                         rasterMosaicStackPreValidatorEntry);
    registerPostValidator("raster_stack",
                          rasterMosaicStackPostValidatorEntry);
}
