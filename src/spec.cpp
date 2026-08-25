#include "spec.h"
#include "embedded.h"
#include "json.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

const ArgSpec *CmdSpec::findLong(const std::string &longName) const
{
    for (const auto &a : args)
    {
        if (a.name == longName)
            return &a;
        for (const auto &al : a.aliases)
            if (al == longName)
                return &a;
        for (const auto &al : a.hiddenAliases)
            if (al == longName)
                return &a;
    }
    return nullptr;
}

const ArgSpec *CmdSpec::findShort(const std::string &shortName) const
{
    for (const auto &a : args)
        for (const auto &s : a.shorts)
            if (s == shortName)
                return &a;
    return nullptr;
}

const ArgSpec *CmdSpec::findByName(const std::string &nameIn) const
{
    for (const auto &a : args)
        if (a.name == nameIn)
            return &a;
    return nullptr;
}

std::string CmdSpec::helpText() const
{
    return embGet("help/" + id + ".txt");
}

std::string CmdSpec::helpErr() const
{
    return embGet("help/" + id + ".err");
}

std::string CmdSpec::jusageText() const
{
    return embGet("jusage/" + id + ".json");
}

std::string CmdSpec::jusageErr() const
{
    return embGet("jusage/" + id + ".err");
}

std::string CmdSpec::errUsage() const
{
    std::string full = embGet("misc/" + id + ".errusage");
    size_t nl = full.find('\n');
    if (nl == std::string::npos)
        return full;
    return full.substr(nl + 1);
}

Spec &Spec::instance()
{
    static Spec spec;
    return spec;
}

Spec::Spec()
{
    std::string treeText = embGet("tree.json");
    bool ok = false;
    JVal root = JVal::parse(treeText, &ok);
    if (!ok)
    {
        fprintf(stderr, "internal error: cannot parse embedded tree.json\n");
        exit(2);
    }
    for (auto &kv : root.obj)
    {
        CmdSpec c;
        c.id = kv.first;
        const JVal &n = kv.second;
        c.name = n.getString("name");
        c.description = n.getString("description");
        c.usageLine = n.getString("usage_line");
        c.path = n.getStringList("path");
        c.subNames = n.getStringList("sub");
        const JVal *argsv = n.get("args");
        if (argsv && argsv->type == JVal::ARRAY)
        {
            for (const auto &av : argsv->arr)
            {
                ArgSpec a;
                a.name = av.getString("name");
                a.description = av.getString("description");
                a.type = av.getString("type");
                a.category = av.getString("category");
                a.metavar = av.getString("metavar");
                a.display = av.getString("display");
                a.section = av.getString("section");
                a.mutex = av.getString("mutual_exclusion_group");
                a.kind = av.getString("kind");
                a.aliases = av.getStringList("aliases");
                a.shorts = av.getStringList("shorts");
                // the reference registers these as hidden aliases: they
                // parse but never appear in help or suggestions
                if (a.name == "metadata")
                    a.hiddenAliases.push_back("mo");
                else if (a.name == "src-crs")
                    a.hiddenAliases.push_back("s_srs");
                else if (a.name == "dst-crs")
                    a.hiddenAliases.push_back("t_srs");
                else if (a.name == "output-layer")
                    a.hiddenAliases.push_back("nln");
                a.choices = av.getStringList("choices");
                a.datasetType = av.getStringList("dataset_type");
                a.inputFlags = av.getStringList("input_flags");
                a.required = av.getBool("required");
                a.packed = av.getBool("packed_values_allowed");
                a.repeated = av.getBool("repeated_arg_allowed");
                const JVal *defv = av.get("default");
                if (defv)
                {
                    a.hasDefault = true;
                    if (defv->type == JVal::BOOL)
                        a.defValue = defv->b ? "true" : "false";
                    else if (defv->type == JVal::STRING)
                        a.defValue = defv->s;
                    else if (defv->type == JVal::INT)
                        a.defValue = strPrintf("%lld", defv->i);
                    else if (defv->type == JVal::DOUBLE)
                        a.defValue = strPrintf("%g", defv->d);
                }
                if (av.get("min_value"))
                {
                    a.hasMin = true;
                    a.minVal = av.getDouble("min_value");
                    a.minIncluded = av.getBool("min_value_is_included", true);
                }
                if (av.get("max_value"))
                {
                    a.hasMax = true;
                    a.maxVal = av.getDouble("max_value");
                    a.maxIncluded = av.getBool("max_value_is_included", true);
                }
                if (av.get("min_count"))
                    a.minCount = av.getInt("min_count");
                if (av.get("max_count"))
                    a.maxCount = av.getInt("max_count");
                const JVal *posv = av.get("positional");
                if (posv && posv->type == JVal::INT)
                    a.positional = static_cast<int>(posv->i);
                c.args.push_back(std::move(a));
            }
        }
        // the reference registers a hidden --update flag on vector
        // update (absent from help/json-usage); update mode defaults on
        if (c.id == "vector_update")
        {
            ArgSpec u;
            u.name = "update";
            u.aliases = {"update"};
            u.type = "boolean";
            u.kind = "input_arguments";
            u.category = "Base";
            u.section = "Options";
            u.display = "--update";
            u.description = "Whether to open existing dataset in update "
                            "mode";
            u.hasDefault = true;
            u.defValue = "false";
            c.args.push_back(std::move(u));
        }
        // hidden -v/--verbose on sozip validate (absent from
        // help/json-usage but accepted by the reference)
        if (c.id == "vsi_sozip_validate")
        {
            ArgSpec v;
            v.name = "verbose";
            v.aliases = {"verbose"};
            v.shorts = {"v"};
            v.type = "boolean";
            v.kind = "input_arguments";
            v.category = "Base";
            v.section = "Options";
            v.display = "-v, --verbose";
            v.description = "Turn on verbose mode";
            v.hasDefault = true;
            v.defValue = "false";
            c.args.push_back(std::move(v));
        }
        cmds.emplace(c.id, std::move(c));
    }
    auto it = cmds.find("ROOT");
    if (it != cmds.end())
    {
        auto &sub = it->second.subNames;
        for (const char *extra : {"convert", "info"})
            if (std::find(sub.begin(), sub.end(), extra) == sub.end())
                sub.push_back(extra);
        std::sort(sub.begin(), sub.end());
    }
}

const CmdSpec *Spec::findById(const std::string &id) const
{
    auto it = cmds.find(id);
    return it == cmds.end() ? nullptr : &it->second;
}
