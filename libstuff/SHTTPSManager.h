#pragma once

class SHTTPSManager : public STCPManager {
  public:
    struct Transaction {
        // Constructor/Destructor
        Transaction(SHTTPSManager& owner_)
          : s(nullptr), created(STimeNow()), finished(0), response(0), owner(owner_) {
        }
        ~Transaction() { SASSERT(!s); }

        // Attributes
        STCPManager::Socket* s;
        uint64_t created;
        uint64_t finished;
        SData fullRequest;
        SData fullResponse;
        int response;
        STable values;
        SHTTPSManager& owner;
    };

    // Constructor/Destructor
    SHTTPSManager();
    SHTTPSManager(const string& pem, const string& srvCrt, const string& caCrt);
    virtual ~SHTTPSManager();

    // Methods
    void closeTransaction(Transaction* transaction);

  public:
    // STCPServer API. Except for postSelect, these are just threadsafe wrappers around base class functions.
    int preSelect(fd_map& fdm);
    void postSelect(fd_map& fdm, uint64_t& nextActivity);
    Socket* openSocket(const string& host, SX509* x509 = nullptr);
    void closeSocket(Socket* socket);

  protected: // Child API
    // Methods
    Transaction* _httpsSend(const string& url, const SData& request);
    Transaction* _createErrorTransaction();
    virtual bool _onRecv(Transaction* transaction) = 0;

  private: // Internal API
    list<Transaction*> _activeTransactionList;
    list<Transaction*> _completedTransactionList;
    SX509* _x509;

    // SHTTPSManager operations are thread-safe, we lock around any accesses to our transaction lists, so that
    // multiple threads can add/remove from them.
    recursive_mutex _listMutex;
};
