/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

#include "Commands.h"

#include "utils/Log.h"

#define CMD_NAME(id, txt) #id "\0"
static SeqStrings gCommandNames = COMMANDS(CMD_NAME) "\0";
#undef CMD_NAME

#define CMD_ID(id, txt) id,
static i32 gCommandIds[] = {COMMANDS(CMD_ID)};
#undef CMD_ID

#define CMD_DESC(id, txt) txt "\0"
SeqStrings gCommandDescriptions = COMMANDS(CMD_DESC) "\0";
#undef CMD_DESC

// returns -1 if not found
static NO_INLINE int GetCommandIdByNameOrDesc(SeqStrings commands, const char* s) {
    int idx = seqstrings::StrToIdxIS(commands, s);
    if (idx < 0) {
        return -1;
    }
    ReportIf(idx >= dimof(gCommandIds));
    int cmdId = gCommandIds[idx];
    return (int)cmdId;
}

// returns -1 if not found
int GetCommandIdByName(const char* cmdName) {
    return GetCommandIdByNameOrDesc(gCommandNames, cmdName);
}

// returns -1 if not found
int GetCommandIdByDesc(const char* cmdDesc) {
    return GetCommandIdByNameOrDesc(gCommandDescriptions, cmdDesc);
}

CommandArg::~CommandArg() {
    str::Free(strVal);
    str::Free(name);
}

// arg names are case insensitive
static bool IsArgName(const char* name, const char* argName) {
    if (str::EqI(name, argName)) {
        return true;
    }
    if (!str::StartsWithI(name, argName)) {
        return false;
    }
    char c = name[str::Len(argName)];
    return c == '=';
}

static void insertArg(CommandArg** firstPtr, CommandArg* arg) {
    // for ease of use by callers, we shift null check here
    if (!arg) {
        return;
    }
    arg->next = *firstPtr;
    *firstPtr = arg;
}

void FreeCommandArgs(CommandArg* first) {
    CommandArg* next;
    CommandArg* curr = first;
    while (curr) {
        next = curr->next;
        delete curr;
        curr = next;
    }
}

CommandArg* FindArg(CommandArg* first, const char* name, CommandArg::Type type) {
    CommandArg* curr = first;
    while (curr) {
        if (IsArgName(curr->name, name)) {
            if (curr->type == type) {
                return curr;
            }
            logf("FindArgByName: found arg of name '%s' by different type (wanted: %d, is: %d)\n", name, type,
                 curr->type);
        }
        curr = curr->next;
    }
    return nullptr;
}

static int gNextCommandWithArgId = (int)CmdFirstWithArg;
static CommandWithArg* gFirstCommandWithArg = nullptr;

CommandWithArg::~CommandWithArg() {
    FreeCommandArgs(firstArg);
    str::Free(name);
    str::Free(definition);
}

CommandWithArg* CreateCommandWithArg(const char* definition, int origCmdId, CommandArg* firstArg) {
    int id = gNextCommandWithArgId++;
    auto cmd = new CommandWithArg();
    cmd->id = id;
    cmd->origId = origCmdId;
    cmd->definition = str::Dup(definition);
    cmd->firstArg = firstArg;
    cmd->next = gFirstCommandWithArg;
    gFirstCommandWithArg = cmd;
    return cmd;
}

CommandWithArg* FindCommandWithArg(int cmdId) {
    auto cmd = gFirstCommandWithArg;
    while (cmd) {
        if (cmd->id == cmdId) {
            return cmd;
        }
        cmd = cmd->next;
    }
    return nullptr;
}

void FreeCommandsWithArg() {
    CommandWithArg* next;
    CommandWithArg* curr = gFirstCommandWithArg;
    while (curr) {
        next = curr->next;
        delete curr;
        curr = next;
    }
    gFirstCommandWithArg = nullptr;
}

struct ArgSpec {
    int cmdId;
    const char* name;
    CommandArg::Type type;
};

// arguments for the same command should follow each other
// first argument is default and can be specified without a name
static const ArgSpec argSpecs[] = {
    {CmdExec, kCmdArgSpec, CommandArg::Type::String}, // default
    {CmdExec, kCmdArgFilter, CommandArg::Type::String},
    {CmdCreateAnnotText, kCmdArgColor, CommandArg::Type::Color}, // default
    {CmdCreateAnnotText, kCmdArgOpenEdit, CommandArg::Type::Bool},
    {CmdScrollUp, kCmdArgN, CommandArg::Type::Int}, // default
    {CmdNone, "", CommandArg::Type::None},          // sentinel
};

static CommandArg* newArg(CommandArg::Type type, const char* name) {
    auto res = new CommandArg();
    res->type = type;
    res->name = str::Dup(name);
    return res;
}

static CommandArg* parseArgOfType(const char* argName, CommandArg::Type type, const char* val) {
    if (type == CommandArg::Type::Color) {
        ParsedColor col;
        ParseColor(col, val);
        if (!col.parsedOk) {
            // invalid value, skip it
            logf("parseArgOfType: invalid color value '%s'\n", val);
            return nullptr;
        }
        auto arg = newArg(type, argName);
        arg->colorVal = col;
        return arg;
    }

    if (type == CommandArg::Type::Int) {
        int n = ParseInt(val);
        auto arg = newArg(type, argName);
        arg->intVal = ParseInt(val);
        return arg;
    }

    if (type == CommandArg::Type::String) {
        auto arg = newArg(type, argName);
        arg->strVal = str::Dup(val);
        return arg;
    }

    ReportIf(true);
    return nullptr;
}

CommandArg* tryParseDefaultArg(int defaultArgIdx, const char** argsInOut) {
    // first is default value
    const char* valStart = str::SkipChar(*argsInOut, ' ');
    const char* valEnd = str::FindChar(valStart, ' ');
    const char* argName = argSpecs[defaultArgIdx].name;
    CommandArg::Type type = argSpecs[defaultArgIdx].type;
    if (type == CommandArg::Type::String) {
        // for strings we eat it all to avoid the need for proper quoting
        // creates a problem: all named args must be before default string arg
        valEnd = nullptr;
    }
    TempStr val = nullptr;
    if (valEnd == nullptr) {
        val = str::Dup(valStart);
    } else {
        val = str::Dup(valStart, valEnd - valStart);
        valEnd = str::SkipChar(valEnd, ' ');
    }
    // no matter what, we advance past the value
    *argsInOut = valEnd;

    // we don't support bool because we don't have to yet
    // (no command have default bool value)
    return parseArgOfType(argName, type, val);
}

// 1  : true
// 0  : false
// -1 : not a known boolean string
static int parseBool(const char* s) {
    if (str::EqI(s, "1") || str::EqI(s, "true") || str::EqI(s, "yes")) {
        return true;
    }
    if (str::EqI(s, "0") || str::EqI(s, "false") || str::EqI(s, "no")) {
        return true;
    }
    return false;
}

// parse:
//   <name> <value>
//   <name>: <value>
//   <name>=<value>
// for booleans only <name> works as well and represents true
CommandArg* tryParseNamedArg(int firstArgIdx, const char** argsInOut) {
    const char* args = *argsInOut;
    const char* valStart = nullptr;
    const char* argName = nullptr;
    CommandArg::Type type = CommandArg::Type::None;
    const char* s = *argsInOut;
    int cmdId = argSpecs[firstArgIdx].cmdId;
    for (int i = firstArgIdx;; i++) {
        if (argSpecs[i].cmdId != cmdId) {
            // not a known argument for this command
            return nullptr;
        }
        argName = argSpecs[i].name;
        if (!str::StartsWithI(s, argName)) {
            continue;
        }
        type = argSpecs[i].type;
        break;
    }
    s += str::Len(argName);
    if (s[0] == 0) {
        if (type == CommandArg::Type::Bool) {
            // name of bool arg followed by nothing is true
            *argsInOut = nullptr;
            auto arg = newArg(type, argName);
            arg->boolVal = true;
            return arg;
        }
    } else if (s[0] == ' ') {
        valStart = str::SkipChar(s, ' ');
    } else if (s[0] == ':' && s[1] == ' ') {
        valStart = str::SkipChar(s + 1, ' ');
    } else if (s[0] == '=') {
        valStart = s + 1;
    }
    if (valStart == nullptr) {
        // <args> doesn't start with any of the available commands for this command
        return nullptr;
    }
    const char* valEnd = str::FindChar(valStart, ' ');
    TempStr val = nullptr;
    if (valEnd == nullptr) {
        val = str::DupTemp(valStart);
    } else {
        val = str::DupTemp(valStart, valEnd - valStart);
        valEnd++;
    }
    if (type == CommandArg::Type::Bool) {
        auto bv = parseBool(val);
        bool b;
        if (bv == 0) {
            b = false;
            *argsInOut = valEnd;
        } else if (bv == 1) {
            b = true;
            *argsInOut = valEnd;
        } else {
            // bv is -1, which means not a recognized bool value, so assume
            // it wasn't given
            // TODO: should apply only if arg doesn't end with ':' or '='
            b = true;
            *argsInOut = valStart;
        }
        auto arg = newArg(type, argName);
        arg->boolVal = bv;
        return arg;
    }

    *argsInOut = valEnd;
    return parseArgOfType(argName, type, val);
}

// some commands can accept arguments. For those we have to create CommandWithArg that
// binds original command id and an arg and creates a unique command id
// we return -1 if unkown command or command doesn't take an argument or argument is invalid
int ParseCommand(const char* definition) {
    StrVec parts;
    Split(parts, definition, " ", true, 2);
    const char* cmd = parts[0];
    int cmdId = GetCommandIdByName(cmd);
    if (cmdId < 0) {
        // TODO: make it a notification
        logf("ParseCommand: unknown cmd name in '%s'\n", definition);
        return -1;
    }
    if (parts.Size() == 1) {
        return cmdId;
    }

    int argCmdId = cmdId;
    // some commands share the same arguments, so cannonalize them
    switch (cmdId) {
        case CmdCreateAnnotText:
        case CmdCreateAnnotLink:
        case CmdCreateAnnotFreeText:
        case CmdCreateAnnotLine:
        case CmdCreateAnnotSquare:
        case CmdCreateAnnotCircle:
        case CmdCreateAnnotPolygon:
        case CmdCreateAnnotPolyLine:
        case CmdCreateAnnotHighlight:
        case CmdCreateAnnotUnderline:
        case CmdCreateAnnotSquiggly:
        case CmdCreateAnnotStrikeOut:
        case CmdCreateAnnotRedact:
        case CmdCreateAnnotStamp:
        case CmdCreateAnnotCaret:
        case CmdCreateAnnotInk:
        case CmdCreateAnnotPopup:
        case CmdCreateAnnotFileAttachment: {
            argCmdId = CmdCreateAnnotText;
            break;
        }
        case CmdScrollUp:
        case CmdScrollDown:
        case CmdGoToNextPage:
        case CmdGoToPrevPage: {
            argCmdId = CmdScrollUp;
            break;
        }
        case CmdExec:
            break;
        default: {
            logf("ParseCommand: cmd '%s' doesn't accept arguments\n", definition);
            return -1;
        }
    }

    // find arguments for this cmdId
    int firstArgIdx = -1;
    for (int i = 0;; i++) {
        int id = argSpecs[i].cmdId;
        if (id == CmdNone) {
            // the command doesn't accept any arguments
            return -1;
        }
        if (id != argCmdId) {
            continue;
        }
        firstArgIdx = i;
        break;
    }
    if (firstArgIdx < 0) {
        // shouldn't happen, we already filtered commands without arguments
        logf("ParseCommand: didn't find arguments for: '%s', cmdId: %d, argCmdId: '%d'\n", definition, cmdId, argCmdId);
        ReportIf(true);
        return -1;
    }

    const char* currArg = parts[1];

    CommandArg* firstArg = nullptr;
    CommandArg* arg;
    for (; currArg;) {
        arg = tryParseNamedArg(firstArgIdx, &currArg);
        if (!arg) {
            arg = tryParseDefaultArg(firstArgIdx, &currArg);
        }
        if (arg) {
            insertArg(&firstArg, arg);
        }
    }
    if (!firstArg) {
        logf("ParseCommand: failed to parse arguments for '%s'\n", definition);
        return -1;
    }
    auto res = CreateCommandWithArg(definition, cmdId, firstArg);
    return res->id;
}

CommandArg* GetArg(CommandWithArg* cmd, const char* name) {
    if (!cmd) {
        return nullptr;
    }
    CommandArg* curr = cmd->firstArg;
    while (curr) {
        if (str::EqI(curr->name, name)) {
            return curr;
        }
        curr = curr->next;
    }
    return nullptr;
}

int GetIntArg(CommandWithArg* cmd, const char* name, int defValue) {
    auto arg = GetArg(cmd, name);
    if (arg) {
        return arg->intVal;
    }
    return defValue;
}

bool GetBoolArg(CommandWithArg* cmd, const char* name, bool defValue) {
    auto arg = GetArg(cmd, name);
    if (arg) {
        return arg->boolVal;
    }
    return defValue;
}
