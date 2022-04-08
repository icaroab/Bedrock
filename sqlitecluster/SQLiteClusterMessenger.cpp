#include <BedrockCommand.h>
#include <sqlitecluster/SQLiteClusterMessenger.h>
#include <sqlitecluster/SQLiteNode.h>
#include <libstuff/SHTTPSManager.h>

#include <unistd.h>
#include <fcntl.h>

SQLiteClusterMessenger::SQLiteClusterMessenger(shared_ptr<SQLiteNode>& node)
 : _node(node)
{
}

void SQLiteClusterMessenger::setErrorResponse(BedrockCommand& command) {
    command.response.methodLine = "500 Internal Server Error";
    command.response.nameValueMap.clear();
    command.response.content.clear();
    command.complete = true;
}

void SQLiteClusterMessenger::shutdownBy(uint64_t shutdownTimestamp) {
    _shutDownBy = shutdownTimestamp;
}

void SQLiteClusterMessenger::reset() {
    _shutDownBy = 0;
}

// Returns true on ready or false on error or timeout.
bool SQLiteClusterMessenger::waitForReady(pollfd& fdspec, uint64_t timeoutTimestamp) {
    static const map <int, string> labels = {
        {POLLOUT, "send"},
        {POLLIN, "recv"},
    };
    string type = "UNKNOWN";
    try {
        type = labels.at(fdspec.events);
    } catch (const out_of_range& e) {}

    while (true) {
        int result = poll(&fdspec, 1, 100); // 100 is timeout in ms.
        if (!result) {
            if (_shutDownBy) {
                SINFO("[HTTPESC] Giving up because shutting down.");
                return false;
            } else if (timeoutTimestamp && timeoutTimestamp < STimeNow()) {
                SINFO("[HTTPESC] Timeout waiting for socket.");
                return false;
            }
            SINFO("[HTTPESC] Socket waiting to be ready (" << type << ").");
        } else if (result == 1) {
            if (fdspec.revents & POLLERR || fdspec.revents & POLLHUP || fdspec.revents & POLLNVAL) {
                SINFO("[HTTPESC] Socket disconnected while waiting to be ready (" << type << ").");
                return false;
            } else if ((fdspec.events & POLLIN && fdspec.revents & POLLIN) || (fdspec.events & POLLOUT && fdspec.revents & POLLOUT)) {
                // Expected case.
                return true;
            } else {
                SWARN("[HTTPESC] Neither error nor success?? (" << type << ").");
                return false;
            }
        } else if (result < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // might work on a second try.
                SWARN("[HTTPESC] poll error (" << type << "): " << errno << ", retrying.");
            } else {
                // Anything else should be fatal.
                SWARN("[HTTPESC] poll error (" << type << "): " << errno);
                return false;
            }
        } else {
            SERROR("[HTTPESC] We have more than 1 file ready????");
        }
    }
}

bool SQLiteClusterMessenger::runOnLeader(BedrockCommand& command) {
    string leaderAddress;
    auto _nodeCopy = atomic_load(&_node);
    if (_nodeCopy) {
        // peerList is const, so we can safely read from it in  multiple threads without locking, similarly,
        // peer->commandAddress is atomic.
        for (SQLiteNode::Peer* peer : _nodeCopy->peerList) {
            string peerCommandAddress = peer->commandAddress;
            if (peer->state == STCPNode::LEADING && !peerCommandAddress.empty()) {
                leaderAddress = peerCommandAddress;
                break;
            }
        }
    }

    // SParseURI expects a typical http or https scheme.
    string url = "http://" + leaderAddress;
    string host, path;
    if (!SParseURI(url, host, path) || !SHostIsValid(host)) {
        return false;
    }

    // Start our escalation timing.
    command.escalationTimeUS = STimeNow();

    // TODO: remove the super-verbose logging before this is in normal production.
    SINFO("[HTTPESC] Socket opening.");
    unique_ptr<SHTTPSManager::Socket> s;
    try {
        // TODO: Future improvement - socket pool so these are reused.
        // TODO: Also, allow S_socket to take a parsed address instead of redoing all the parsing above.
        s = unique_ptr<SHTTPSManager::Socket>(new SHTTPSManager::Socket(host, nullptr));
    } catch (const SException& exception) {
        // Finish our escalation.
        command.escalationTimeUS = STimeNow() - command.escalationTimeUS;
        SINFO("[HTTPESC] Socket failed to open.");
        return false;
    }
    SINFO("[HTTPESC] Socket opened.");

    // This is what we need to send.
    SData request = command.request;
    request.nameValueMap["ID"] = command.id;
    SFastBuffer buf(request.serialize());

    // We only have one FD to poll.
    pollfd fdspec = {s->s, POLLOUT, 0};
    while (true) {
        if (!waitForReady(fdspec, command.timeout())) {
            return false;
        }

        ssize_t bytesSent = send(s->s, buf.c_str(), buf.size(), 0);
        if (bytesSent == -1) {
            switch (errno) {
                case EAGAIN:
                case EINTR:
                    // these are ok. try again.
                    SINFO("[HTTPESC] Got error (send): " << errno << ", trying again.");
                    break;
                default:
                    SINFO("[HTTPESC] Got error (send): " << errno << ", fatal.");
                    return false;
            }
        } else {
            buf.consumeFront(bytesSent);
            if (buf.empty()) {
                // Everything has sent, we're done with this loop.
                break;
            }
        }
    }

    // If we fail before here, we can try again. If we fail after here, we should return an error.

    // Ok, now we need to receive the response.
    fdspec.events = POLLIN;
    string responseStr;
    char response[4096] = {0};
    while (true) {
        if (!waitForReady(fdspec, command.timeout())) {
            setErrorResponse(command);
            return false;
        }

        ssize_t bytesRead = recv(s->s, response, 4096, 0);
        if (bytesRead == -1) {
            switch (errno) {
                case EAGAIN:
                case EINTR:
                    // these are ok. try again.
                    SINFO("[HTTPESC] Got error (recv): " << errno << ", trying again.");
                    break;
                default:
                    SINFO("[HTTPESC] Got error (recv): " << errno << ", fatal.");
                    setErrorResponse(command);
                    return false;
            }
        } else if (bytesRead == 0) {
            SINFO("[HTTPESC] disconnected.");
            setErrorResponse(command);
            return false;
        } else {
            // Save the response.
            responseStr.append(response, bytesRead);

            // Are we done? We've only sent one command so we can only get one response.
            int size = SParseHTTP(responseStr, command.response.methodLine, command.response.nameValueMap, command.response.content);
            if (size) {
                break;
            }
        }
    }

    // If we got here, the command is complete.
    command.complete = true;

    // Finish our escalation timing.
    command.escalationTimeUS = STimeNow() - command.escalationTimeUS;

    return true;
}