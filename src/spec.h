#pragma once
#include <map>
#include <string>
#include <vector>

struct ArgSpec
{
    std::string name;
    std::string description;
    std::string type;  // boolean,string,integer,real,string_list,integer_list,real_list,dataset,dataset_list
    std::string category;
    std::string metavar;
    std::string display;
    std::string section;
    std::string mutex;
    std::string kind;  // input_arguments/output_arguments/input_output_arguments
    std::vector<std::string> aliases;
    std::vector<std::string> hiddenAliases;  // match but never suggested
    std::vector<std::string> shorts;
    std::vector<std::string> choices;
    std::vector<std::string> datasetType;
    std::vector<std::string> inputFlags;
    bool required = false;
    bool packed = false;
    bool repeated = false;
    bool hasDefault = false;
    std::string defValue;
    bool hasMin = false, hasMax = false;
    double minVal = 0, maxVal = 0;
    bool minIncluded = true, maxIncluded = true;
    long long minCount = -1, maxCount = -1;
    int positional = -1;

    bool isBool() const { return type == "boolean"; }
    bool isList() const
    {
        return type == "string_list" || type == "integer_list" ||
               type == "real_list" || type == "dataset_list";
    }
    bool isDataset() const
    {
        return type == "dataset" || type == "dataset_list";
    }
};

struct CmdSpec
{
    std::string id;  // "ROOT" or "raster_convert"
    std::string name;
    std::string description;
    std::string usageLine;
    std::vector<std::string> path;
    std::vector<std::string> subNames;
    std::vector<ArgSpec> args;

    bool leaf() const { return subNames.empty(); }
    const ArgSpec *findLong(const std::string &longName) const;
    const ArgSpec *findShort(const std::string &shortName) const;
    const ArgSpec *findByName(const std::string &name) const;
    std::string helpText() const;
    std::string helpErr() const;
    std::string jusageText() const;
    std::string jusageErr() const;
    std::string errUsage() const;  // for non-leaf: usage block after ERROR line
};

class Spec
{
  public:
    static Spec &instance();
    const CmdSpec *findById(const std::string &id) const;
    std::map<std::string, CmdSpec> cmds;

  private:
    Spec();
};
