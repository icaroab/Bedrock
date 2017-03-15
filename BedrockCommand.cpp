#include <libstuff/libstuff.h>
#include "BedrockCommand.h"

BedrockCommand::BedrockCommand() :
    SQLiteCommand(),
    httpsRequest(nullptr),
    priority(PRIORITY_NORMAL),
    peekCount(0),
    processCount(0)
{ }

// Constructor by moving from a SQLiteCommand.
BedrockCommand::BedrockCommand(SQLiteCommand&& from) :
    SQLiteCommand(std::move(from)),
    httpsRequest(nullptr),
    priority(PRIORITY_NORMAL),
    peekCount(0),
    processCount(0)
{
    _init();
}

BedrockCommand::BedrockCommand(SData&& _request) :
    SQLiteCommand(move(request)),
    httpsRequest(nullptr),
    priority(PRIORITY_NORMAL),
    peekCount(0),
    processCount(0)
{
    _init();
}

BedrockCommand::BedrockCommand(SData _request) :
    SQLiteCommand(move(_request)),
    httpsRequest(nullptr),
    priority(PRIORITY_NORMAL),
    peekCount(0),
    processCount(0)
{
    _init();
}

BedrockCommand& BedrockCommand::operator=(BedrockCommand&& from)
{
    if (this != &from) {
        httpsRequest = from.httpsRequest;
        from.httpsRequest = nullptr;
        SQLiteCommand::operator=(move(from));
    }

    return *this;
}

void BedrockCommand::_init()
{
    // Initialize the priority, if supplied.
    if (request.isSet("priority")) {
        int tempPriority = request.calc("priority");
        switch (tempPriority) {
            // For any valid case, we just set the value directly.
            case BedrockCommand::PRIORITY_MIN:
            case BedrockCommand::PRIORITY_LOW:
            case BedrockCommand::PRIORITY_NORMAL:
            case BedrockCommand::PRIORITY_HIGH:
            case BedrockCommand::PRIORITY_MAX:
                priority = static_cast<Priority>(tempPriority);
                break;
            default:
                // But an invalid case gets set to NORMAL, and a warning is thrown.
                SWARN("'" << request.methodLine << "' requested invalid priority: " << tempPriority);
                priority = PRIORITY_NORMAL;
                break;
        }
    }
}
