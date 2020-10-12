#Documented Doichain Changes from Namecoin 0.20

## the following changes where done when we forked from name_coin. it should serve as a documentation.

- mempool.h 
	- definition of map<valtype, std::set<uint256>> mapNameDoi
	- registersDoi
	- clear
	
- mempool.cpp (is called when a transaction hits the mempool) 
	- check 
	- addUnchecked (105)
	- remove (155) - removes a mempoolentry from mapNameDois
	- checkTx


WORKFLOW
- new name tx
	- consensus/tx_verify.cpp - CheckTxInputs -> CheckNameTransaction
	- 
	
	
TODO:
- mempool.cpp 
	- removeConflicts - please check if we can resove any possible db conflicts here for doi
	
	