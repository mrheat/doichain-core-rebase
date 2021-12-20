// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_RPC_WALLET_H
#define BITCOIN_WALLET_RPC_WALLET_H

#include <span.h>
#include <wallet/walletutil.h>

#include <vector>

class CCoinControl;
class CRecipient;
class CRPCCommand;
class CTxIn;
class CWallet;

Span<const CRPCCommand> GetWalletRPCCommands();

/* These are private to rpcwallet.cpp upstream, but are used also from
   rpcnames.cpp in Namecoin.  */
UniValue SendMoney(CWallet& wallet, const CCoinControl& coin_control,
                   const CTxIn* withInput,
                   std::vector<CRecipient>& recipients, mapValue_t map_value, bool verbose);

#endif // BITCOIN_WALLET_RPC_WALLET_H
