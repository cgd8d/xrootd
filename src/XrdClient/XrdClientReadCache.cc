//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClientReadCache                                                   // 
//                                                                      //
// Author: Fabrizio Furano (INFN Padova, 2006)                          //
//                                                                      //
// Classes to handle cache reading and cache placeholders               //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//       $Id$

#include "XrdClient/XrdClientReadCache.hh"
#include "XrdOuc/XrdOucPthread.hh"
#include "XrdClient/XrdClientDebug.hh"
#include "XrdClient/XrdClientEnv.hh"


//________________________________________________________________________
XrdClientReadCacheItem::XrdClientReadCacheItem(const void *buffer, long long begin_offs,
					       long long end_offs, long long ticksnow, bool placeholder)
{
    // Constructor
    fIsPlaceholder = placeholder;

    fData = (void *)0;
    if (!fIsPlaceholder) 
	fData = (void *)buffer;

    Touch(ticksnow);
    fBeginOffset = begin_offs;
    fEndOffset = end_offs;
}

//________________________________________________________________________
XrdClientReadCacheItem::~XrdClientReadCacheItem()
{
    // Destructor

    if (fData)
	free(fData);
}

//
// XrdClientReadCache
//

//________________________________________________________________________
long long XrdClientReadCache::GetTimestampTick()
{
    // Return timestamp

    // Mutual exclusion man!
    XrdOucMutexHelper mtx(fMutex);
    return ++fTimestampTickCounter;
}
  
//________________________________________________________________________
XrdClientReadCache::XrdClientReadCache()
{
    // Constructor

    fTimestampTickCounter = 0;
    fTotalByteCount = 0;

    fMissRate = 0.0;
    fMissCount = 0;
    fReadsCounter = 0;

    fBytesSubmitted = 0;
    fBytesHit = 0;
    fBytesUsefulness = 0.0;

    fMaxCacheSize = EnvGetLong(NAME_READCACHESIZE);
}

//________________________________________________________________________
XrdClientReadCache::~XrdClientReadCache()
{
    // Destructor

    RemoveItems();

}



//________________________________________________________________________
void XrdClientReadCache::SubmitRawData(const void *buffer, long long begin_offs,
					long long end_offs)
{
    if (!buffer) return;
    XrdClientReadCacheItem *itm;

    Info(XrdClientDebug::kHIDEBUG, "Cache",
	 "Submitting " << begin_offs << "->" << end_offs << " to cache.");

    // Mutual exclusion man!
    XrdOucMutexHelper mtx(fMutex);


    // We remove all the blocks contained in the one we are going to put
    RemoveItems(begin_offs, end_offs);

    if (MakeFreeSpace(end_offs - begin_offs)) {
	itm = new XrdClientReadCacheItem(buffer, begin_offs, end_offs,
					 GetTimestampTick());


	// We find the correct insert position to keep the list sorted by
	// BeginOffset
	// A data block will always be inserted BEFORE a true block with
	// equal beginoffset
	int pos = 0;
	for (pos = 0; pos < fItems.GetSize(); pos++) {
	    if (fItems[pos]->ContainsInterval(begin_offs, end_offs)) {
		pos = -1;
		break;
	    }
	    if (fItems[pos]->BeginOffset() >= begin_offs)
		break;
	}

	if (pos >= 0) {
	    fItems.Insert(itm, pos);
	    fTotalByteCount += itm->Size();
	    fBytesSubmitted += itm->Size();
	}
	else delete itm;


    } // if


    //PrintCache();
}


//________________________________________________________________________
void XrdClientReadCache::SubmitXMessage(XrdClientMessage *xmsg, long long begin_offs,
					long long end_offs)
{
    // To populate the cache of items, newly received

    const void *buffer = xmsg->DonateData();

    SubmitRawData(buffer, begin_offs, end_offs);
}

//________________________________________________________________________
void XrdClientReadCache::PutPlaceholder(long long begin_offs,
					long long end_offs)
{
    // To put a placeholder into the cache

    XrdClientReadCacheItem *itm;

    itm = new XrdClientReadCacheItem(0, begin_offs, end_offs,
				     GetTimestampTick(), true);

    {
	// Mutual exclusion man!
	XrdOucMutexHelper mtx(fMutex);

	// We find the correct insert position to keep the list sorted by
	// BeginOffset
	int pos = 0;
	for (pos = 0; pos < fItems.GetSize(); pos++) {
	    if (fItems[pos]->ContainsInterval(begin_offs, end_offs)) {
		delete itm;
		return;
	    }

	    if (fItems[pos]->BeginOffset() >= begin_offs)
		break;
	}

	fItems.Insert(itm, pos);

    } // if
}

//________________________________________________________________________
long XrdClientReadCache::GetDataIfPresent(const void *buffer,
					  long long begin_offs,
					  long long end_offs,
					  bool PerfCalc, 
					  XrdClientIntvList &missingblks,
					  long &outstandingblks)
{
    // Copies the requested data from the cache. False if not possible
    // Also, this function figures out if:
    // - there are data blocks marked as outstanding
    // - there are sub blocks which should be requested

    int it;
    long bytesgot = 0;

    long long lasttakenbyte = begin_offs-1;
    outstandingblks = 0;
    missingblks.Clear();

    XrdOucMutexHelper mtx(fMutex);

    if (PerfCalc)
	fReadsCounter++;

    // We try to compose the requested data block by concatenating smaller
    //  blocks. 


    // Find a block helping us to go forward
    // The blocks are sorted
    // By scanning the list we also look for:
    //  - the useful blocks which are outstanding
    //  - the useful blocks which are missing, and not outstanding

    // First scan: we get the useful data
    // and remember where we arrived
    for (it = 0; it < fItems.GetSize(); it++) {
	long l = 0;

	if (!fItems[it]) continue;

	if (fItems[it]->BeginOffset() > lasttakenbyte+1) break;

	if (!fItems[it]->IsPlaceholder())
	    l = fItems[it]->GetPartialInterval(((char *)buffer)+bytesgot,
					       begin_offs+bytesgot, end_offs);
	else break;

	if (l > 0) {
	    bytesgot += l;
	    lasttakenbyte = begin_offs+bytesgot-1;

	    fItems[it]->Touch(GetTimestampTick());

	    if (PerfCalc) {
		fBytesHit += l;
		UpdatePerfCounters();
	    }

	    if (bytesgot >= end_offs - begin_offs + 1) {
		return bytesgot;
	    }

	}

    }


    // We are here if something is missing to get all the data we need
    // Hence we build a list of what is missing
    // right now what is missing is the interval
    // [lasttakenbyte+1, end_offs]

    XrdClientCacheInterval intv;


    for (; it < fItems.GetSize(); it++) {
	long l;

	if (fItems[it]->BeginOffset() > end_offs) break;

	if (fItems[it]->BeginOffset() > lasttakenbyte+1) {
	    // We found that the interval
	    // [lastbyteseen+1, fItems[it]->BeginOffset-1]
	    // is a hole, which should be requested explicitly

	    intv.beginoffs = lasttakenbyte+1;
	    intv.endoffs = fItems[it]->BeginOffset()-1;
	    missingblks.Push_back( intv );

	    lasttakenbyte = fItems[it]->EndOffset();
	    if (lasttakenbyte >= end_offs) break;
	    continue;
	}

	// Let's see if we can get something from this blk, even if it's a placeholder
	l = fItems[it]->GetPartialInterval(0, lasttakenbyte+1, end_offs);

	if (l > 0) {
	    // We found a placeholder to wait for
	    // or a data block

	    if (fItems[it]->IsPlaceholder()) {
		// Add this interval to the number of blocks to wait for
		outstandingblks++;

	    }


	    lasttakenbyte += l;
	}


    }

//     if (lasttakenbyte+1 < end_offs) {
// 	    intv.beginoffs = lasttakenbyte+1;
// 	    intv.endoffs = end_offs;
// 	    missingblks.Push_back( intv );
//     }


    if (PerfCalc) {
	fMissCount++;
	UpdatePerfCounters();
    }

    return bytesgot;
}


//________________________________________________________________________
void XrdClientReadCache::PrintCache() {

    XrdOucMutexHelper mtx(fMutex);
    int it;

    Info(XrdClientDebug::kHIDEBUG, "Cache",
	 "Cache Status --------------------------");

    for (it = 0; it < fItems.GetSize(); it++) {

	if (fItems[it]) {

	    if (fItems[it]->IsPlaceholder()) {
		
		Info(XrdClientDebug::kHIDEBUG,
		     "Cache blk", it << "Placeholder " <<
		     fItems[it]->BeginOffset() << "->" << fItems[it]->EndOffset() );

	    }
	    else
		Info(XrdClientDebug::kHIDEBUG,
		     "Cache blk", it << "Data block  " <<
		     fItems[it]->BeginOffset() << "->" << fItems[it]->EndOffset() );

	}
    }
    
    Info(XrdClientDebug::kHIDEBUG, "Cache",
	 "--------------------------------------");

}


//________________________________________________________________________
void XrdClientReadCache::RemoveItems(long long begin_offs, long long end_offs)
{
    // To remove all the items contained in the given interval

    int it;
    XrdOucMutexHelper mtx(fMutex);

    it = 0;
    // We remove all the blocks contained in the given interval
    while (it < fItems.GetSize())
	if (fItems[it] &&
	    fItems[it]->ContainedInInterval(begin_offs, end_offs)) {

	    if (!fItems[it]->IsPlaceholder())
		fTotalByteCount -= fItems[it]->Size();
	    
	    delete fItems[it];
	    fItems.Erase(it);
	}
	else it++;

    // Then we resize or split the placeholders overlapping the given interval
    bool changed;
    do {
	changed = false;
	for (it = 0; it < fItems.GetSize(); it++) {


	    if (fItems[it] &&
		fItems[it]->IsPlaceholder() ) {
		long long plc1_beg = 0;
		long long plc1_end = 0;
	  
		long long plc2_beg = 0;
		long long plc2_end = 0;
	  
		// We have a placeholder which contains the arrived block
		plc1_beg = fItems[it]->BeginOffset();
		plc1_end = begin_offs-1;

		plc2_beg = end_offs+1;
		plc2_end = fItems[it]->EndOffset();

		if ( ( (begin_offs >= fItems[it]->BeginOffset()) &&
		       (begin_offs <= fItems[it]->EndOffset()) ) ||
		     ( (end_offs >= fItems[it]->BeginOffset()) &&
		       (end_offs <= fItems[it]->EndOffset()) ) ) {

		    delete fItems[it];
		    fItems.Erase(it);
		    changed = true;

		    if (plc1_end - plc1_beg > 32) {
			PutPlaceholder(plc1_beg, plc1_end);
		    }

		    if (plc2_end - plc2_beg > 32) {
			PutPlaceholder(plc2_beg, plc2_end);
		    }

		    break;
	  
		}

		


	    }

	}

    } while (changed);



}

//________________________________________________________________________
void XrdClientReadCache::RemoveItems()
{
    // To remove all the items
    int it;
    XrdOucMutexHelper mtx(fMutex);

    it = 0;

    while (it < fItems.GetSize()) {
	delete fItems[it];
	fItems.Erase(it);
    }

    fTotalByteCount = 0;

}
//________________________________________________________________________
void XrdClientReadCache::RemovePlaceholders() {

    // Finds the LRU item and removes it
    // We don't remove placeholders

    int it;

    XrdOucMutexHelper mtx(fMutex);

    if (!fItems.GetSize()) return;

    while (1) {

	if (fItems[it] && fItems[it]->IsPlaceholder()) {
	    delete fItems[it];
	    fItems.Erase(it);
	}
	else
	    it++;

	if (it == fItems.GetSize()) break;
    }

}

//________________________________________________________________________
bool XrdClientReadCache::RemoveLRUItem()
{
    // Finds the LRU item and removes it
    // We don't remove placeholders

    int it, lruit;
    long long minticks = -1;
    XrdClientReadCacheItem *item;

    XrdOucMutexHelper mtx(fMutex);

    lruit = -1;
    for (it = 0; it < fItems.GetSize(); it++) {
	// We don't remove placeholders
	if (fItems[it] && !fItems[it]->IsPlaceholder()) {
	    if ((minticks < 0) || (fItems[it]->GetTimestampTicks() < minticks)) {
		minticks = fItems[it]->GetTimestampTicks();
		lruit = it;
	    }      
	}
    }

    if (lruit >= 0)
	item = fItems[lruit];
    else return false;

    if (minticks >= 0) {
	fTotalByteCount -= item->Size();
	delete item;
	fItems.Erase(lruit);
    }

    return false;
}

//________________________________________________________________________
bool XrdClientReadCache::MakeFreeSpace(long long bytes)
{
    // False if not possible (requested space exceeds max size!)

    if (!WillFit(bytes))
	return false;

    XrdOucMutexHelper mtx(fMutex);

    while (fMaxCacheSize - fTotalByteCount < bytes)
	RemoveLRUItem();

    return true;
}
