// Mumoria - the Bank, Guild Hall, Auction House and Trade screens, drawn the
// way Umoria draws its stores (store.cpp): a full page of lettered lines,
// prompts on the message line, ESC to leave. No new windows.

#pragma once

#include "session.h"

#include <string>

struct MuTrade {
    MuSession *side[2]{};
    std::vector<int> slots[2];
    int64_t gold[2]{};
    bool confirmed[2]{};
    int state = 0; // 0 open, 1 done, 2 called off
    std::string note;
    int refs = 2;
};

void mu_trade_request(MuSession *s, const std::string &name);
void mu_trade_screen(MuSession *s);
void mu_trade_cancel(MuSession *s);

// Ends auctions whose time is up. Database only: any thread.
void mu_auction_settle();
