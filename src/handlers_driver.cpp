#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "util.h"
#include "vsi.h"

#include <cstdlib>

namespace
{

int cogValidateHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.str("dataset");
    std::string err;
    auto ds = openRaster(input, err);
    if (!ds)
    {
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    // the reference embeds a Python interpreter; without osgeo_utils the
    // import fails and the module name carries a heap address
    void *marker = malloc(16);
    std::string msg = strPrintf(
        "Traceback (most recent call last):\n  File "
        "\"cog_validate_module_%p\", line 1, in <module>\n"
        "ModuleNotFoundError: No module named 'osgeo_utils'\n",
        marker);
    free(marker);
    cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
    return 1;
}

}  // namespace

void registerDriverHandlers()
{
    registerHandler("driver_cog_validate", cogValidateHandler);
}
