#pragma once
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>

using namespace std;

class BedrockConflictManagerCommandInfo {
  public:
    size_t count = 0;
    map<string, size_t> tableUseCounts;
};

class BedrockConflictManager {
  public:
    BedrockConflictManager();
    void recordTables(const string& commandName, const set<string>& tables);
    string generateReport();

  private:
    mutex m;
    map<string, BedrockConflictManagerCommandInfo> _commandInfo;
};

class PageLockGuard {
  public:
    PageLockGuard(int64_t page);
    ~PageLockGuard();

  private:
    struct MPair {
        int64_t count = 1;
        mutex m;
    };

    // For controlling access to internals.
    static mutex controlMutex;
    static map<int64_t, MPair> mutexCounts;
    static list<int64_t> mutexOrder;
    int64_t _page;
    mutex* _mref = nullptr;
};
