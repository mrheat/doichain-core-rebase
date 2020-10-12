#Documented Doichain Changes from Namecoin 0.20

## the following changes where done when we forked from name_coin. it should serve as a documentation.

- mempool.h 
	- definition of map<valtype, std::set<uint256>> mapNameDoi
- mempool.cpp (is called when a transaction hits the mempool) 
	- check 
	- addUnchecked (105)
	- remove (155) - removes a mempoolentry from mapNameDois
	
	
	
TODO:
- mempool.cpp 
	- removeConflicts - please check if we can resove any possible db conflicts here for doi
	
	