// Copyright (c) 2014-2020 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <names/main.h>
#include <script/standard.h>
#include <key_io.h>

#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <hash.h>
#include <names/encoding.h>
#include <names/common.h>
#include <names/main.h>
#include <script/interpreter.h>
#include <script/names.h>
#include <txmempool.h>
#include <uint256.h>
#include <undo.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <validation.h>

#include <string>
#include <vector>

namespace
{

/**
 * Check whether a name at nPrevHeight is expired at nHeight.  Also
 * heights of MEMPOOL_HEIGHT are supported.  For nHeight == MEMPOOL_HEIGHT,
 * we check at the current best tip's height.
 * @param nPrevHeight The name's output.
 * @param nHeight The height at which it should be updated.
 * @return True iff the name is expired.
 */
bool
isExpired (unsigned nPrevHeight, unsigned nHeight)
{
  assert (nHeight != MEMPOOL_HEIGHT);
  if (nPrevHeight == MEMPOOL_HEIGHT)
    return false;

  const Consensus::Params& params = Params ().GetConsensus ();
  return nPrevHeight + params.rules->NameExpirationDepth (nHeight) <= nHeight;
}

} // anonymous namespace

/* ************************************************************************** */
/* CNameData.  */

bool
CNameData::isExpired (unsigned h) const
{
  return ::isExpired (nHeight, h);
}

/* ************************************************************************** */
/* CNameTxUndo.  */

void
CNameTxUndo::fromOldState (const valtype& nm, const CCoinsView& view)
{
  name = nm;
  isNew = !view.GetName (name, oldData);
}

void
CNameTxUndo::apply (CCoinsViewCache& view) const
{
  if (isNew)
    view.DeleteName (name);
  else
    view.SetName (name, oldData, true);
}

/* ************************************************************************** */

bool
CheckNameTransaction (const CTransaction& tx, unsigned nHeight,
                      const CCoinsView& view,
                      TxValidationState& state, unsigned flags)
{

  const bool fMempool = (flags & SCRIPT_VERIFY_NAMES_MEMPOOL);

  /* Ignore historic bugs
  CChainParams::BugType type;
  if (Params ().IsHistoricBug (tx.GetHash (), nHeight, type))
    return true;.  */

  /* As a first step, try to locate inputs and outputs of the transaction
     that are name scripts.  At most one input and output should be
     a name operation.  */
  LogPrintf ("CheckNameTransaction: Checking Inputs - 0\n");
  int nameIn = -1;
  CNameScript nameOpIn;
  Coin coinIn;
  for (unsigned i = 0; i < tx.vin.size (); ++i)
    {
      const COutPoint& prevout = tx.vin[i].prevout;
      Coin coin;
      if (!view.GetCoin (prevout, coin))
        return state.Invalid (TxValidationResult::TX_MISSING_INPUTS,
                              "bad-txns-inputs-missingorspent",
                              "Failed to fetch name input coin");

      const CNameScript op(coin.out.scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameIn != -1)
            return state.Invalid (TxValidationResult::TX_CONSENSUS,
                                  "tx-multiple-name-inputs",
                                  "Multiple name inputs");
          nameIn = i;
          nameOpIn = op;
          coinIn = coin;
        }
    }
  LogPrintf ("CheckNameTransaction: Checking Outputs - 1\n");
  int nameOut = -1;
  CNameScript nameOpOut;
  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameOut != -1)
            return state.Invalid (TxValidationResult::TX_CONSENSUS,
                                  "tx-multiple-name-outputs",
                                  "Multiple name outputs");
          nameOut = i;
          nameOpOut = op;
        }
    }

  /* Check that no name inputs/outputs are present for a non-Doichain tx.
     If that's the case, all is fine.  For a Doichain tx instead, there
     should be at least an output (for NAME_NEW, no inputs are expected).  */

  if (!tx.IsDoichain ())
    {
      if (nameIn != -1)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nonname-with-name-input",
                              "Non-name transaction has name input");
      if (nameOut != -1)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nonname-with-name-output",
                              "Non-name transaction has name output");
      return true;
    }
  LogPrintf ("CheckNameTransaction Step 2\n");
  assert (tx.IsDoichain ());
  if (nameOut == -1)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-name-without-name-output",
                          "Name transaction has no name output");

  /* Reject "greedy names".  */
  const Consensus::Params& params = Params ().GetConsensus ();
  if (tx.vout[nameOut].nValue < params.rules->MinNameCoinAmount(nHeight))
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-name-greedy",
                          "Greedy name operation");

  /* Handle NAME_NEW now, since this is easy and different from the other
     operations.  */

  if (nameOpOut.getNameOp () == OP_NAME_NEW)
    {
      if (nameIn != -1)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-namenew-with-name-input",
                              "NAME_NEW with previous name input");

      if (nameOpOut.getOpHash ().size () != 20)
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-namenew-wrong-size",
                              "NAME_NEW's hash has the wrong size");

      return true;
    }

  /* Now that we have ruled out NAME_NEW, check that we have a previous
     name input that is being updated.  */
  LogPrintf ("CheckNameTransaction Step 3\n");
  assert (nameOpOut.isAnyUpdate () || nameOpOut.isDoiRegistration ());
  //TODO invalid block=1173d2615de4aba9785646bc414040e622cc04869593f006872b9013e1b1201b  height=29966 mainnet why is that
  /*if (nameIn == -1) 
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-nameupdate-without-name-input",
                          "Name update has no previous name input"); */
  const valtype& name = nameOpOut.getOpName ();

  if (name.size () > MAX_NAME_LENGTH)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-name-invalid",
                          "Invalid name");
  if (nameOpOut.getOpValue ().size () > MAX_VALUE_LENGTH)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-value-invalid",
                          "Invalid value");

  /* Process NAME_UPDATE next.  */
  LogPrintf ("CheckNameTransaction Step 4\n");
  if (nameOpOut.getNameOp () == OP_NAME_UPDATE)
    {
      if (!nameOpIn.isAnyUpdate ())
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-invalid-prev",
                              "Name input for NAME_UPDATE is not an update");

      if (name != nameOpIn.getOpName ())
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-name-mismatch",
                              "NAME_UPDATE name mismatch to name input");

      /* If the name input is pending, then no further checks with respect
         to the name input in the name database are done.  Otherwise, we verify
         that the name input matches the name database; this is redundant
         as UTXO handling takes care of it anyway, but we do it for
         an extra safety layer.  */
      const unsigned inHeight = coinIn.nHeight;
      if (inHeight == MEMPOOL_HEIGHT)
        return true;

      CNameData oldName;
      if (!view.GetName (name, oldName))
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-nonexistant",
                              "NAME_UPDATE name does not exist");
      if (oldName.isExpired (nHeight))
        return state.Invalid (TxValidationResult::TX_CONSENSUS,
                              "tx-nameupdate-expired",
                              "NAME_UPDATE on an expired name");
      assert (inHeight == oldName.getHeight ());
      assert (tx.vin[nameIn].prevout == oldName.getUpdateOutpoint ());

      return true;
    }
  LogPrintf ("CheckNameTransaction Step 5\n");
  if (nameOpOut.getNameOp () == OP_NAME_DOI)
    {
	   if (nameIn != -1){
		   LogPrintf ("this OP_NAME_DOI WITH previous name input !\n");
		      CNameData oldName;
		      if (!view.GetName (name, oldName))
		        return state.Invalid (TxValidationResult::TX_CONSENSUS,
		                              "tx-nameupdate-nonexistant",
		                              "NAME_DOI name does not exist");

			   if (nameOpIn.getNameOp () != OP_NAME_DOI)
			     return state.Invalid (TxValidationResult::TX_CONSENSUS,
			                           "tx-name-doi-not-name-doi-input",
			                           "NAME_DOI input is not a OP_NAME_DOI");
	   }else{ 
       //here we create a new name_doi in case it is a not used nameId and MAYBE even if it is a used nameId (need to check!)
       //in case it is an already used nameId we need to 
       LogPrintf ("this OP_NAME_DOI WITHOUT previous name input !\n");
       CNameData oldName;
       CTxDestination destOld;
       std::string addrStrOld;
       CTxDestination destNew;
       std::string addrStrNew;
       const unsigned inHeight = coinIn.nHeight;
       if (view.GetName(name, oldName)) {

  	       if (ExtractDestination (oldName.getAddress(), destOld)){
    			addrStrOld = EncodeDestination (destOld);
               		LogPrintf ("oldName: %s\n",  addrStrOld);
	       }

  	       if (ExtractDestination (nameOpOut.getAddress(), destNew)){
    			addrStrNew = EncodeDestination (destNew);
               		LogPrintf ("newName: %s\n",  addrStrNew);
	       }

              //if(strcmp(addrStrNew,addrStrOld)){
              //if(addrStrNew!=addrStrOld){ //problem is here that most name_dois have a new destination after overwrite
              if(inHeight>170000){ //blockheight were we do not except name_doi overwriting anymore
                LogPrintf("this OP_NAME_DOI already exists and cannot be overwritten!\n");
                return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                   "tx-name-doi-name-used",
                                   "NAME_DOI name is already used - please use correct inputs if its an name_doi update");
              }
       }
      LogPrintf ("this OP_NAME_DOI does not yet exists planing to add it !\n");
      /*const unsigned inHeight = coinIn.nHeight;
		  if (inHeight == MEMPOOL_HEIGHT)
		       return true;

		   assert (inHeight == oldName.getHeight ());
		   assert (tx.vin[nameIn].prevout == oldName.getUpdateOutpoint ()); */

       return true;
		 /* 
		   LogPrintf ("this OP_NAME_DOI WITHOUT previous name input no check needed here!\n");

		  CNameData oldName;
		  if (!view.GetName (name, oldName))
			return state.Invalid (TxValidationResult::TX_CONSENSUS,
								  "tx-nameupdate-nonexistant",
								  "OP_NAME_DOI name does not exist");

	      if (oldName.isExpired (nHeight))
	        return state.Invalid (TxValidationResult::TX_CONSENSUS,
	                              "tx-nameupdate-expired",
	                              "OP_NAME_DOI on an expired name");*/
	   }

      return true;      
    }
  /* Finally, NAME_FIRSTUPDATE.  */
  assert (nameOpOut.getNameOp () == OP_NAME_FIRSTUPDATE);
  
  if (nameOpIn.getNameOp () != OP_NAME_NEW)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-nonnew-input",
                          "NAME_FIRSTUPDATE input is not a NAME_NEW"); 

  /* Maturity of NAME_NEW is checked only if we're not adding
     to the mempool.  */
  if (!fMempool)
    {
      assert (static_cast<unsigned> (coinIn.nHeight) != MEMPOOL_HEIGHT);
      if (coinIn.nHeight + MIN_FIRSTUPDATE_DEPTH > nHeight)
        return state.Invalid (TxValidationResult::TX_PREMATURE_SPEND,
                              "tx-firstupdate-immature",
                              "NAME_FIRSTUPDATE on immature NAME_NEW");
    }

  if (nameOpOut.getOpRand ().size () > 20)
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-invalid-rand",
                          "NAME_FIRSTUPDATE rand value is too large");

  {
    valtype toHash(nameOpOut.getOpRand ());
    toHash.insert (toHash.end (), name.begin (), name.end ());
    const uint160 hash = Hash160 (toHash);
    if (hash != uint160 (nameOpIn.getOpHash ()))
      return state.Invalid (TxValidationResult::TX_CONSENSUS,
                            "tx-firstupdate-hash-mismatch",
                            "NAME_FIRSTUPDATE mismatch in hash / rand value");
  }
  LogPrintf ("CheckNameTransaction Step 6\n");
  CNameData oldName;
  if (view.GetName (name, oldName) && !oldName.isExpired (nHeight))
    return state.Invalid (TxValidationResult::TX_CONSENSUS,
                          "tx-firstupdate-existing-name",
                          "NAME_FIRSTUPDATE on existing name");

  /* We don't have to specifically check that miners don't create blocks with
     conflicting NAME_FIRSTUPDATE's, since the mining's CCoinsViewCache
     takes care of this with the check above already.  */

  return true;
}

void
ApplyNameTransaction (const CTransaction& tx, unsigned nHeight,
                      CCoinsViewCache& view, CBlockUndo& undo)
{
  assert (nHeight != MEMPOOL_HEIGHT);

  /* Handle historic bugs that should *not* be applied.  Names that are
     outputs should be marked as unspendable in this case.  Otherwise,
     we get an inconsistency between the UTXO set and the name database.
  CChainParams::BugType type;
  const uint256 txHash = tx.GetHash ();
  if (Params ().IsHistoricBug (txHash, nHeight, type)
      && type != CChainParams::BUG_FULLY_APPLY)
    {
      if (type == CChainParams::BUG_FULLY_IGNORE)
        for (unsigned i = 0; i < tx.vout.size (); ++i)
          {
            const CNameScript op(tx.vout[i].scriptPubKey);
            if (op.isNameOp () && op.isAnyUpdate ())
              view.SpendCoin (COutPoint (txHash, i));
          }
      return;
    }  */

  /* This check must be done *after* the historic bug fixing above!  Some
     of the names that must be handled above are actually produced by
     transactions *not* marked as Doichain tx.  
  if (!tx.IsDoichain ())
    return;*/

  /* Changes are encoded in the outputs.  We don't have to do any checks,
     so simply apply all these.  */

  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp () && op.isAnyUpdate ())
        {
          const valtype& name = op.getOpName ();
          LogPrint (BCLog::NAMES, "Updating name at height %d: %s\n",
                    nHeight, EncodeNameForMessage (name));

          CNameTxUndo opUndo;
          opUndo.fromOldState (name, view);
          undo.vnameundo.push_back (opUndo);

          CNameData data;
          data.fromScript (nHeight, COutPoint (tx.GetHash (), i), op);
          view.SetName (name, data, false);
        }
    }
}

bool
ExpireNames (unsigned nHeight, CCoinsViewCache& view, CBlockUndo& undo,
             std::set<valtype>& names)
{
  names.clear ();

  /* The genesis block contains no name expirations.  */
  if (nHeight == 0)
    return true;

  /* Otherwise, find out at which update heights names have expired
     since the last block.  If the expiration depth changes, this could
     be multiple heights at once.  */

  const Consensus::Params& params = Params ().GetConsensus ();
  const unsigned expDepthOld = params.rules->NameExpirationDepth (nHeight - 1);
  const unsigned expDepthNow = params.rules->NameExpirationDepth (nHeight);

  if (expDepthNow > nHeight)
    return true;

  /* Both are inclusive!  The last expireTo was nHeight - 1 - expDepthOld,
     now we start at this value + 1.  */
  const unsigned expireFrom = nHeight - expDepthOld;
  const unsigned expireTo = nHeight - expDepthNow;

  /* It is possible that expireFrom = expireTo + 1, in case that the
     expiration period is raised together with the block height.  In this
     case, no names expire in the current step.  This case means that
     the absolute expiration height "n - expirationDepth(n)" is
     flat -- which is fine.  */
  assert (expireFrom <= expireTo + 1);

  /* Find all names that expire at those depths.  Note that GetNamesForHeight
     clears the output set, to we union all sets here.  */
  for (unsigned h = expireFrom; h <= expireTo; ++h)
    {
      std::set<valtype> newNames;
      view.GetNamesForHeight (h, newNames);
      names.insert (newNames.begin (), newNames.end ());
    }

  /* Expire all those names.  */
  for (std::set<valtype>::const_iterator i = names.begin ();
       i != names.end (); ++i)
    {
      const std::string nameStr = EncodeNameForMessage (*i);

      CNameData data;
      if (!view.GetName (*i, data))
        return error ("%s : name %s not found in the database",
                      __func__, nameStr);
      if (!data.isExpired (nHeight))
        return error ("%s : name %s is not actually expired",
                      __func__, nameStr);

      /* Special rule:  When d/postmortem expires (the name used by
         libcoin in the name-stealing demonstration), it's coin
         is already spent.  Ignore.  */
      if (nHeight == 175868
            && EncodeName (*i, NameEncoding::ASCII) == "d/postmortem")
        continue;

      const COutPoint& out = data.getUpdateOutpoint ();
      Coin coin;
      if (!view.GetCoin(out, coin))
        return error ("%s : name coin for %s is not available",
                      __func__, nameStr);
      const CNameScript nameOp(coin.out.scriptPubKey);
      if (!nameOp.isNameOp () || !nameOp.isAnyUpdate ()
          || nameOp.getOpName () != *i)
        return error ("%s : name coin to be expired is wrong script", __func__);

      if (!view.SpendCoin (out, &coin))
        return error ("%s : spending name coin failed", __func__);
      undo.vexpired.push_back (coin);
    }

  return true;
}

bool
UnexpireNames (unsigned nHeight, CBlockUndo& undo, CCoinsViewCache& view,
               std::set<valtype>& names)
{
  names.clear ();

  /* The genesis block contains no name expirations.  */
  if (nHeight == 0)
    return true;

  std::vector<Coin>::reverse_iterator i;
  for (i = undo.vexpired.rbegin (); i != undo.vexpired.rend (); ++i)
    {
      const CNameScript nameOp(i->out.scriptPubKey);
      if (!nameOp.isNameOp () || !nameOp.isAnyUpdate ())
        return error ("%s : wrong script to be unexpired", __func__);

      const valtype& name = nameOp.getOpName ();
      if (names.count (name) > 0)
        return error ("%s : name %s unexpired twice",
                      __func__, EncodeNameForMessage (name));
      names.insert (name);

      CNameData data;
      if (!view.GetName (nameOp.getOpName (), data))
        return error ("%s : no data for name '%s' to be unexpired",
                      __func__, EncodeNameForMessage (name));
      if (!data.isExpired (nHeight) || data.isExpired (nHeight - 1))
        return error ("%s : name '%s' to be unexpired is not expired in the DB"
                      " or it was already expired before the current height",
                      __func__, EncodeNameForMessage (name));

      if (ApplyTxInUndo (std::move(*i), view,
                         data.getUpdateOutpoint ()) != DISCONNECT_OK)
        return error ("%s : failed to undo name coin spending", __func__);
    }

  return true;
}

void
CheckNameDB (CChainState& chainState, bool disconnect)
{
  const int option
    = gArgs.GetIntArg ("-checknamedb", Params ().DefaultCheckNameDB ());

  if (option == -1)
    return;

  assert (option >= 0);
  if (option != 0)
    {
      if (disconnect || chainState.m_chain.Height () % option != 0)
        return;
    }

  auto& coinsTip = chainState.CoinsTip ();
  coinsTip.Flush ();
  const bool ok = coinsTip.ValidateNameDB (chainState, [] () {});

  /* The DB is inconsistent (mismatch between UTXO set and names DB) between
     (roughly) blocks 139,000 and 180,000.  This is caused by libcoin's
     "name stealing" bug.  For instance, d/postmortem is removed from
     the UTXO set shortly after registration (when it is used to steal
     names), but it remains in the name DB until it expires.  */
  if (!ok)
    {
      const unsigned nHeight = chainState.m_chain.Height ();
      LogPrintf ("ERROR: %s : name database is inconsistent\n", __func__);
      if (nHeight >= 139000 && nHeight <= 180000)
        LogPrintf ("This is expected due to 'name stealing'.\n");
      else
        assert (false);
    }
}
