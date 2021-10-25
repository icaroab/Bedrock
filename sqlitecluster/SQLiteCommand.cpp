#include "SQLiteCommand.h"

#include <libstuff/libstuff.h>
#include <libstuff/SRandom.h>
#include <iostream>

SData SQLiteCommand::preprocessRequest(SData&& request) {
    // If the request doesn't specify an execution time, default to right now.
    if (!request.isSet("commandExecuteTime")) {
        request["commandExecuteTime"] = to_string(STimeNow());
    }

    // Add a request ID if one was missing.
    if (!request.isSet("requestID")) {
        string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        string requestID;
        for (int i = 0; i < 6; i++) {
            requestID += chars[SRandom::rand64() % chars.size()];
        }
        request["requestID"] = requestID;
    }
    return move(request);
}

SQLiteCommand::SQLiteCommand(SData&& _request) : 
    initiatingPeerID(0),
    initiatingClientID(0),
    _requestPtr(make_unique<SData>(move(preprocessRequest(move(_request))))),
    request(*_requestPtr),
    writeConsistency(SQLiteNode::ASYNC),
    complete(false),
    escalationTimeUS(0),
    creationTime(STimeNow()),
    escalated(false)
{
    // Initialize the consistency, if supplied.
    if (request.isSet("writeConsistency")) {
        int tempConsistency = request.calc("writeConsistency");
        switch (tempConsistency) {
            // For any valid case, we just set the value directly.
            case SQLiteNode::ASYNC:
            case SQLiteNode::ONE:
            case SQLiteNode::QUORUM:
                writeConsistency = static_cast<SQLiteNode::ConsistencyLevel>(tempConsistency);
                break;
            default:
                // But an invalid case gets set to ASYNC, and a warning is thrown.
                SWARN("'" << request.methodLine << "' requested invalid consistency: " << writeConsistency);
                writeConsistency = SQLiteNode::ASYNC;
                break;
        }
    }
}

SQLiteCommand::SQLiteCommand(SQLiteCommand&& from) : 
    initiatingPeerID(from.initiatingPeerID),
    initiatingClientID(from.initiatingClientID),
    id(move(from.id)),
    _requestPtr(move(from._requestPtr)),
    request(*_requestPtr),
    jsonContent(move(from.jsonContent)),
    response(move(from.response)),
    writeConsistency(from.writeConsistency),
    complete(from.complete),
    escalationTimeUS(from.escalationTimeUS),
    creationTime(from.creationTime),
    escalated(from.escalated)
{
}

SQLiteCommand::SQLiteCommand() :
    initiatingPeerID(0),
    initiatingClientID(0),
    _requestPtr(make_unique<SData>()),
    request(*_requestPtr),
    writeConsistency(SQLiteNode::ASYNC),
    complete(false),
    escalationTimeUS(0),
    creationTime(STimeNow()),
    escalated(false)
{ }
