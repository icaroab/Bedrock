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
SQLiteClusterMessenger::WaitForReadyResult SQLiteClusterMessenger::waitForReady(pollfd& fdspec, uint64_t timeoutTimestamp) {
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
                return WaitForReadyResult::SHUTTING_DOWN;
            } else if (timeoutTimestamp && timeoutTimestamp < STimeNow()) {
                SINFO("[HTTPESC] Timeout waiting for socket.");
                return WaitForReadyResult::TIMEOUT;
            }
        } else if (result == 1) {
            if (fdspec.revents & POLLERR || fdspec.revents & POLLHUP || fdspec.revents & POLLNVAL) {
                SINFO("[HTTPESC] Socket disconnected while waiting to be ready (" << type << ").");
                // This case in particular happens if we try and escalate to a leader with a closed command port. Maybe
                // we should wait and retry?
                return fdspec.events == POLLIN ? WaitForReadyResult::DISCONNECTED_IN : WaitForReadyResult::DISCONNETED_OUT;
            } else if ((fdspec.events & POLLIN && fdspec.revents & POLLIN) || (fdspec.events & POLLOUT && fdspec.revents & POLLOUT)) {
                // Expected case.
                return WaitForReadyResult::OK;
            } else {
                SWARN("[HTTPESC] Neither error nor success?? (" << type << ").");
                return WaitForReadyResult::UNSPECIFIED;
            }
        } else if (result < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // might work on a second try.
                SWARN("[HTTPESC] poll error (" << type << "): " << errno << ", retrying.");
            } else {
                // Anything else should be fatal.
                SWARN("[HTTPESC] poll error (" << type << "): " << errno);
                return WaitForReadyResult::POLL_ERROR;
            }
        } else {
            SERROR("[HTTPESC] We have more than 1 file ready????");
        }
    }
}

bool SQLiteClusterMessenger::runOnLeader(BedrockCommand& command) {
    // Ok, we want to retry this for up to 5 seconds if we can't get the address.
    // Ideally, we let SQLiteNode notify us of changes here, but we can probably just wait for now.
    string leaderAddress = _node->leaderCommandAddress();
    if (leaderAddress.empty()) {
        SINFO("[HTTPESC] No leader address.");
        return false;
    }

    // So the above needs a few things to run correctly:
    // 1. There must be a leader.
    // 2. It must actually be *leading*, not standingdown.
    // 3. It's port must be opened, this is not the case as it begins shutting down.
    //
    // Because the way we create sockets is non-blocking, the creation never fails, we fail in attempting to send.
    //
    // When we fail to connect cause leader's command  port is closed, we get:
    // 2022-04-21T22:10:22.317146+00:00 db2.lax bedrock: 6GLDGj sps.workforce@collectivesolution.net (SQLiteClusterMessenger.cpp:53) waitForReady [worker87] [info] [HTTPESC] Socket disconnected while waiting to be ready (recv).

    // SParseURI expects a typical http or https scheme.
    string url = "http://" + leaderAddress;
    string host, path;
    if (!SParseURI(url, host, path) || !SHostIsValid(host)) {
        return false;
    }

    // Start our escalation timing.
    command.escalationTimeUS = STimeNow();

    // TODO: remove the super-verbose logging before this is in normal production.
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

    // This is what we need to send.
    SData request = command.request;
    request.nameValueMap["ID"] = command.id;
    SFastBuffer buf(request.serialize());

    // We only have one FD to poll.
    pollfd fdspec = {s->s, POLLOUT, 0};
    while (true) {
        if (waitForReady(fdspec, command.timeout()) != WaitForReadyResult::OK) {
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
        if (waitForReady(fdspec, command.timeout()) != WaitForReadyResult::OK) {
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
    command.escalated = true;

    // Finish our escalation timing.
    command.escalationTimeUS = STimeNow() - command.escalationTimeUS;

    return true;
}
