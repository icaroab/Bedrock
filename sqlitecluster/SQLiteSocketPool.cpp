#include "SQLiteSocketPool.h"

SQLiteSocketPool::SQLiteSocketPool(const string& host)
  : host(host),
    _timeoutThread(&SQLiteSocketPool::_timeoutThreadFunc, this) {
}

SQLiteSocketPool::~SQLiteSocketPool() {
    {
        unique_lock<mutex> lock(_poolMutex);
        _exit = true;
    }
    _poolCV.notify_one();
    _timeoutThread.join();
}

void SQLiteSocketPool::_timeoutThreadFunc() {
    while (true) {
        unique_lock<mutex> lock(_poolMutex);

        // If `exit` is set, we are done.
        if (_exit) {
            return;
        }

        // Prune any sockets that expired already.
        auto now = chrono::steady_clock::now();
        auto last = _sockets.begin();
        while (((last->first + timeout) < now) && last != _sockets.end()) {
            last++;
        }

        // This calls the destructor for each item in the list, closing the sockets.
        _sockets.erase(_sockets.begin(), last);

        // If there are still sockets, the next wakeup is `timeout` after the first one.
        if (_sockets.size()) {
            _poolCV.wait_until(lock, _sockets.front().first + timeout);
        } else {
            // If there are no more sockets, we sleep until we're interrupted.
            _poolCV.wait(lock);
        }
    }
}

unique_ptr<STCPManager::Socket> SQLiteSocketPool::getSocket() {
    {
        // If there's an existing socket, return it.
        lock_guard<mutex> lock(_poolMutex);
        if (_sockets.size()) {
            pair<chrono::steady_clock::time_point, unique_ptr<STCPManager::Socket>> s = move(_sockets.front());
            _sockets.pop_front();
            return move(s.second);
        }
    }

    // If we get here, we need to create a socket to return. No need to hold the lock, so it goes out of scope.
    try {
        // TODO: Allow S_socket to take a parsed address instead of redoing all the parsing above.
        return unique_ptr<STCPManager::Socket>(new STCPManager::Socket(host, nullptr));
    } catch (const SException& exception) {
        return nullptr;
    }
}

void SQLiteSocketPool::returnSocket(unique_ptr<STCPManager::Socket>&& s) {
    {
        lock_guard<mutex> lock(_poolMutex);
        _sockets.emplace_back(make_pair(chrono::steady_clock::now(), move(s)));
    }

    // Notify the waiting thread that we have something for it to do in 10s.
    _poolCV.notify_one();
}
