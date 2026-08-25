#pragma once
#include <string>
#include <vector>

// `gdal completion <words...>`; words[0] is the completed program name.
int runCompletion(const std::vector<std::string> &words);
