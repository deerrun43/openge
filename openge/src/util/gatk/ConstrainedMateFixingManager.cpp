/*********************************************************************
 *
 * ConstrainedMateFixingManager.cpp: Port of GATK's ConstrainedMateFixingManager.
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 5 May 2012
 *
 *********************************************************************
 *
 * This file has been ported from GATK's implementation in Java, and
 * is released under the Virginia Tech Non-Commercial Purpose License.
 * A copy of this license has been provided in  the openge/ directory.
 * 
 * The original file, ConstrainedMateFixinigManager.java, was released 
 * under the following license:
 *
 * Copyright (c) 2010 The Broad Institute
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
 * THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * A locally resorting, mate fixing sam file writer that supports an idiom where reads are only moved around if
 * the ISIZE of the pair is < X and reads are not allowed to move any more than Y bp from their original positions.
 *
 * To understand this data structure, let's begin by asking -- when are we certain we know the position of read R added
 * to the writer and its mate M given that R has been added to the writer (but M may not be), their ISIZE in R, at the
 * moment that a read K is added to the writer, under the constraints X and Y?  Complex I know.  First, because
 * reads cannot move more than Y bp in either direction, we know that R originated at most R.pos + Y bp from its
 * current position.  Also, we know that K is at most K.pos + Y bp from it's original position.  If R is maximally
 * shifted to the right, and K shifted to the left, then they could at most move 2Y together.  So if the distance
 * between R and K > 2Y, we know that there are no reads left in the original stream that could be moved before R.
 *
 * Now, we also need to be certain if we have a mate pair M, that won't emit R before we can incorporate any move of
 * M into the mate pair info R.  There are two cases to consider here:
 *
 * If ISIZE > X, we know that we won't move M when we see it, so we can safely emit R knowing that
 * M is fixed in place.
 *
 * If ISIZE <= X, M might be moved, and it we have to wait until we see M in the stream to know it's position.
 * So R must be buffered until either M arrives, or we see a read K that's more than 2Y units past the original position
 * of M.
 *
 * So the worst-case memory consumption here is proportional to the number of reads
 * occurring between R and M + 2 Y, and so is proportional to the depth of the data and X and Y.
 *
 * This leads to the following simple algorithm:
 *
 * addAlignment(newRead):
 *   addReadToListOfReads(newRead)
 *   update mate pair of newRead if present in list of reads
 *
 *   for ( read in list of reads [in order of increasing read.pos] ):
 *     if read.pos < newRead.pos - 2Y && (read.isize >= X || read.matePos < newRead.pos - 2 * Y):
 *        emit read and remove from list of reads
 *     else:
 *        break
 *
 * @author depristo, ebanks
 * @version 0.2
 */

#include "ConstrainedMateFixingManager.h"
#include "GenomeLocParser.h"

#include "../../algorithms/local_realignment.h"

#include "../bamtools/Sort.h"

#include <map>
#include <string>
#include <set>
#include <list>
#include <vector>
#include <functional>
#include <algorithm>
#include <sstream>
using namespace std;

size_t getReferenceLength(const OGERead & read) {
    size_t length = 0;

    vector<CigarOp> cigar = read.getCigarData();
    for (vector<CigarOp>::const_iterator i = cigar.begin(); i != cigar.end(); i++) {
        switch (i->type) {
            case 'M':
            case 'D':
            case 'N':
            case '=':
            case 'X':
                length += i->length;
                break;
        }
    }
    return length;
}

int getEndPosition(const OGERead & read) {
    return read.getPosition() + getReferenceLength(read) - 1;
}

//////////////
//These functions (computeInsertSize and setMateInfo) are from SamPairUtil.java

int computeInsertSize(const OGERead & firstEnd, const OGERead & secondEnd) {
    if (!firstEnd.IsMapped() || !secondEnd.IsMapped()) {
        return 0;
    }
    if (!firstEnd.getRefID() == secondEnd.getRefID()) {
        return 0;
    }

    //std::bitset<8> x(firstEnd.AlignmentFlag ^ secondEnd.AlignmentFlag );

    const int firstEnd5PrimePosition = firstEnd.IsReverseStrand()? getEndPosition(firstEnd): firstEnd.getPosition();
    const int secondEnd5PrimePosition = secondEnd.IsReverseStrand()? getEndPosition(secondEnd): secondEnd.getPosition();

    const int adjustment = (secondEnd5PrimePosition >= firstEnd5PrimePosition) ? +1 : -1;
    //cerr << "IS: " << firstEnd.Position + 1 << " " << (firstEnd5PrimePosition + 1) << " " << (secondEnd5PrimePosition + 1) << " adj " << adjustment << " is " << (secondEnd5PrimePosition - firstEnd5PrimePosition + adjustment) << endl;
    return secondEnd5PrimePosition - firstEnd5PrimePosition + adjustment;
}

void setMateInfo( OGERead & rec1, OGERead & rec2) {
    const int NO_ALIGNMENT_REFERENCE_INDEX = -1;
    const int NO_ALIGNMENT_START = -1;
    // If neither read is unmapped just set their mate info
    if (rec1.IsMapped() && rec2.IsMapped()) {
        rec1.setMateRefID(rec2.getMateRefID());
        rec1.setMatePosition(rec2.getPosition());
        rec1.SetIsMateReverseStrand(rec2.IsReverseStrand());
        rec1.SetIsMateMapped(true);
        rec1.AddTag("MQ", "S", rec2.getMapQuality());
        
        rec2.setMateRefID(rec1.getRefID());
        rec2.setMatePosition(rec1.getPosition());
        rec2.SetIsMateReverseStrand( rec1.IsReverseStrand() );
        rec2.SetIsMateMapped(true);
        rec2.AddTag("MQ", "S", rec1.getMapQuality());
    }
    // Else if they're both unmapped set that straight
    else if (!rec1.IsMapped() && !rec2.IsMapped()) {
        rec1.setRefID(NO_ALIGNMENT_REFERENCE_INDEX);
        rec1.setPosition(NO_ALIGNMENT_START);
        rec1.setMateRefID(NO_ALIGNMENT_REFERENCE_INDEX);
        rec1.setMatePosition(NO_ALIGNMENT_START);
        rec1.SetIsMateReverseStrand(rec2.IsReverseStrand());
        rec1.SetIsMateMapped(false);
        rec2.RemoveTag("MQ");
        
        rec2.setRefID(NO_ALIGNMENT_REFERENCE_INDEX);
        rec2.setPosition(NO_ALIGNMENT_START);
        rec2.setMateRefID(NO_ALIGNMENT_REFERENCE_INDEX);
        rec2.setMatePosition(NO_ALIGNMENT_START);
        rec2.SetIsMateReverseStrand(rec1.IsReverseStrand());
        rec2.SetIsMateMapped(true);
        rec2.RemoveTag("MQ");
    }
    // And if only one is mapped copy it's coordinate information to the mate
    else {
        OGERead & mapped   = rec1.IsMapped() ? rec1 : rec2;
        OGERead & unmapped = rec1.IsMapped() ? rec2 : rec1;
        unmapped.setRefID(mapped.getRefID());
        unmapped.setPosition(mapped.getPosition());

        mapped.setMateRefID(unmapped.getRefID());
        mapped.setMatePosition(unmapped.getPosition());
        mapped.SetIsMateReverseStrand(unmapped.IsReverseStrand());
        mapped.SetIsMateMapped(false);

        unmapped.setMateRefID(mapped.getRefID());
        unmapped.setMatePosition(mapped.getPosition());
        unmapped.SetIsMateReverseStrand(mapped.IsReverseStrand());
        unmapped.SetIsMateMapped(true);
    }
    
    int insertSize = computeInsertSize(rec1, rec2);
    if(insertSize > 0) insertSize--;
    if(insertSize < 0) insertSize++;
    rec1.setInsertSize(insertSize);
    rec2.setInsertSize(-insertSize);
}

OGERead * ConstrainedMateFixingManager::remove(waitingReads_t & treeSet) {
    OGERead * first = *treeSet.begin();
    assert(!treeSet.empty());
    treeSet.erase(treeSet.begin());
    return first;
}


//private SimpleTimer timer = new SimpleTimer("ConstrainedWriter");
//private long PROGRESS_PRINT_FREQUENCY = 10 * 1000;             // in milliseconds
//private long lastProgressPrintTime = -1;                       // When was the last time we printed progress log?


/**
 *
 * @param writer                                 actual writer
 * @param genomeLocParser                        the GenomeLocParser object
 * @param maxInsertSizeForMovingReadPairs        max insert size allowed for moving pairs
 * @param maxMoveAllowed                         max positional move allowed for any read
 * @param maxRecordsInMemory                     max records to keep in memory
 */
ConstrainedMateFixingManager::ConstrainedMateFixingManager( LocalRealignment * writer, const int maxInsertSizeForMovingReadPairs, const int maxMoveAllowed, const int maxRecordsInMemory, GenomeLocParser * loc_parser)
: output_module(writer)
, maxInsertSizeForMovingReadPairs(maxInsertSizeForMovingReadPairs)
, MAX_POS_MOVE_ALLOWED(maxMoveAllowed)
, MAX_RECORDS_IN_MEMORY(maxRecordsInMemory)
, lastLocFlushed(NULL)
, counter(0)
, loc_parser(loc_parser)
{
    if(!output_module->isNothreads()) {
        int ret = pthread_create(&add_read_thread, NULL, addread_threadproc, this);
        if(0 != ret) {
            cerr << "Error creating ConstrainedMateFixingManager thread. Aborting. (error " << ret << ")." << endl;
            exit(-1);
        }
    }
}

ConstrainedMateFixingManager::~ConstrainedMateFixingManager() {
}

bool ConstrainedMateFixingManager::canMoveReads(const GenomeLoc & earliestPosition) const {
    if ( DEBUG ) cerr << "Refusing to realign? " << earliestPosition.toString() << " vs. " << lastLocFlushed << endl;
    
    return lastLocFlushed == NULL ||
    lastLocFlushed->compareContigs(earliestPosition) != 0 ||
    lastLocFlushed->distance(earliestPosition) > maxInsertSizeForMovingReadPairs;
}

bool ConstrainedMateFixingManager::noReadCanMoveBefore(int pos, const OGERead & addedRead) const {
    return pos + 2 * MAX_POS_MOVE_ALLOWED < addedRead.getPosition();
}

void * ConstrainedMateFixingManager::addread_threadproc(void * data)
{
    ConstrainedMateFixingManager * manager = (ConstrainedMateFixingManager *)data;
    
    ogeNameThread("CMFMaddRead");
    
    while(true) {
        cmfm_read_t r = manager->addReadQueue.pop();
        
        if(r.read == NULL)
            break;
        
        manager->addReadInternal(r.read, r.readWasModified, r.canFlush);
    }
    
    return 0;
}

void ConstrainedMateFixingManager::addReads(const vector<OGERead *> & newReads, const set<OGERead *> & modifiedReads) {
	if(!output_module->isNothreads())
        add_read_lock.lock();

    for (vector<OGERead *>::const_iterator newRead =  newReads.begin(); newRead != newReads.end(); newRead++ ) {
        cmfm_read_t r;
        r.read = *newRead;
        r.readWasModified = modifiedReads.count(*newRead) > 0;
        r.canFlush = false;

        if(output_module->isNothreads())
            addReadInternal(r.read, r.readWasModified, r.canFlush);
        else
            addReadQueue.push(r);
    }

	if(!output_module->isNothreads())
        add_read_lock.unlock();
}

void ConstrainedMateFixingManager::addRead(OGERead * newRead, bool readWasModified, bool canFlush) {
    cmfm_read_t r;
    r.read = newRead;
    r.readWasModified = readWasModified;
    r.canFlush = canFlush;
	if(!output_module->isNothreads())
        add_read_lock.lock();
    
    if(output_module->isNothreads())
        addReadInternal(r.read, r.readWasModified, r.canFlush);
    else
        addReadQueue.push(r);
    
	if(!output_module->isNothreads())
        add_read_lock.unlock();
}

void ConstrainedMateFixingManager::addReadInternal( OGERead * newRead, const bool readWasModified, bool canFlush ) {
    if ( DEBUG ) {
        string OP;
        newRead->GetTag("OP", OP);
        cerr << "New read pos " << newRead->getPosition() << " OP = " << OP << " " << readWasModified << endl;
    }

    // if the new read is on a different contig or we have too many reads, then we need to flush the queue and clear the map
    bool tooManyReads = (waitingReads.size() >= MAX_RECORDS_IN_MEMORY);
    if ( (canFlush && tooManyReads) || (waitingReads.size() > 0 && (*waitingReads.begin())->getRefID() != newRead->getRefID()) ) {
        if ( DEBUG ) {
            stringstream ss("");
            if(tooManyReads)
                ss << "too many reads";
            else
                ss << "move to new contig #: " << newRead->getRefID() << " from #" << (*waitingReads.begin())->getRefID();
            cerr << "Flushing queue on " << ss.str() << " at " << newRead->getPosition() << endl;
        }
        
        while ( waitingReads.size() > 1 ) {
            // emit to disk
            writeRead(remove(waitingReads));
        }
        
        OGERead * lastRead = remove(waitingReads);
        lastLocFlushed = (lastRead->getRefID() == -1) ? NULL : new GenomeLoc(loc_parser->createGenomeLoc(*lastRead));
        writeRead(lastRead);
        
        if ( !tooManyReads )
            forMateMatching.clear();
        else
            purgeUnmodifiedMates();
    }
    
    // fix mates, as needed
    // Since setMateInfo can move reads, we potentially need to remove the mate, and requeue
    // it to ensure proper sorting
    if ( newRead->IsPaired() ) {
        if ( forMateMatching.count(newRead->getName()) ) {
            SAMRecordHashObject & mate = forMateMatching.find(newRead->getName())->second;

            // 1. Frustratingly, Picard's setMateInfo() method unaligns (by setting the reference contig
            // to '*') read pairs when both of their flags have the unmapped bit set.  This is problematic
            // when trying to emit reads in coordinate order because all of a sudden we have reads in the
            // middle of the bam file that now belong at the end - and any mapped reads that get emitted
            // after them trigger an exception in the writer.  For our purposes, because we shouldn't be
            // moving read pairs when they are both unmapped anyways, we'll just not run fix mates on them.
            // 2. Furthermore, when reads get mapped to the junction of two chromosomes (e.g. MT since it
            // is actually circular DNA), their unmapped bit is set, but they are given legitimate coordinates.
            // The Picard code will come in and move the read all the way back to its mate (which can be
            // arbitrarily far away).  However, we do still want to move legitimately unmapped reads whose
            // mates are mapped, so the compromise will be that if the mate is still in the queue then we'll
            // move the read and otherwise we won't.
            bool doNotFixMates = !newRead->IsMapped() && (!mate.record->IsMapped() || !waitingReads.count(mate.record));
            if ( !doNotFixMates ) {
                
                bool reQueueMate = !mate.record->IsMapped() && newRead->IsMapped();
                if ( reQueueMate ) {
                    // the mate was unmapped, but newRead was mapped, so the mate may have been moved
                    // to be next-to newRead, so needs to be reinserted into the waitingReads queue
                    // note -- this must be called before the setMateInfo call below
                    if ( ! waitingReads.erase(mate.record) )
                        // we must have hit a region with too much depth and flushed the queue
                        reQueueMate = false;
                }
                
                // we've already seen our mate -- set the mate info and remove it from the map
                setMateInfo(*mate.record, *newRead);
                if ( reQueueMate ) waitingReads.insert(mate.record);
            }

            forMateMatching.erase(newRead->getName());
        } else if ( pairedReadIsMovable(*newRead) ) {
            forMateMatching[newRead->getName()] = SAMRecordHashObject(newRead, readWasModified);

        }
    }
    
    waitingReads.insert(newRead);
    
    if ( ++counter % EMIT_FREQUENCY == 0 ) {
        while ( ! waitingReads.empty() ) { // there's something in the queue
            OGERead * read = *waitingReads.begin();

            if ( noReadCanMoveBefore(read->getPosition(), *newRead) &&
                (!pairedReadIsMovable(*read)                               // we won't try to move such a read
                 || noReadCanMoveBefore(read->getMatePosition(), *newRead ) ) ) { // we're already past where the mate started
                    
                    // remove reads from the map that we have emitted -- useful for case where the mate never showed up
                    forMateMatching.erase(read->getName());
                    
                    if ( DEBUG ) {
                        string OP;
                        newRead->GetTag("OP", OP);
                        cerr << "EMIT!  At " << newRead->getPosition() << ": read " << read->getName() << " at " <<  read->getPosition() << " with isize " << read->getLength() << ", mate start " << read->getMatePosition() << ", op = " << OP << endl;
                    }
                    // emit to disk
                    writeRead(remove(waitingReads));
                } else {
                    if ( DEBUG )
                        cerr << "At " << newRead->getPosition() << ": read " << read->getName() << " at " << read->getPosition() << " with isize " << read->getLength() << " couldn't be emited, mate start " << read->getMatePosition() << endl;
                    break;
                }
        }
        
        if ( DEBUG ) cerr << "At " << newRead->getPosition() << ": Done with emit cycle" << endl;
    }
}

void ConstrainedMateFixingManager::writeRead(OGERead * read) const {
    output_module->writeRead(read);
}

/**
 * @param read  the read
 * @return true if the read shouldn't be moved given the constraints of this SAMFileWriter
 */
bool ConstrainedMateFixingManager::iSizeTooBigToMove(const OGERead & read) const {
    return iSizeTooBigToMove(read, maxInsertSizeForMovingReadPairs);               // we won't try to move such a read
}

bool ConstrainedMateFixingManager::iSizeTooBigToMove(const OGERead & read, int maxInsertSizeForMovingReadPairs) {
    return ( read.IsPaired() && read.IsMapped() && read.getRefID() != read.getMateRefID() ) // maps to different chromosomes
    || abs(read.getLength()) > maxInsertSizeForMovingReadPairs;     // we won't try to move such a read
}

void ConstrainedMateFixingManager::purgeUnmodifiedMates() {
    map<string, SAMRecordHashObject> forMateMatchingCleaned;
    for (map<string, SAMRecordHashObject>::const_iterator entry = forMateMatching.begin(); entry != forMateMatching.end(); entry++) {
        if ( entry->second.wasModified )
            forMateMatchingCleaned[entry->first] = entry->second;
    }
    
    forMateMatching.clear(); // explicitly clear the memory
    forMateMatching = forMateMatchingCleaned;
}

bool ConstrainedMateFixingManager::pairedReadIsMovable(const OGERead & read) const {
    return read.IsPaired()                                          // we're a paired read
    && (read.IsMapped() || read.IsMateMapped())  // at least one read is mapped
    && !iSizeTooBigToMove(read);                                     // insert size isn't too big
    
}

void ConstrainedMateFixingManager::close() {
    
    //close the thread:
    cmfm_read_t r = {0};
    addReadQueue.push(r);
    
    if(!output_module->isNothreads()) {
        int ret = pthread_join(add_read_thread, NULL);
        if(0 != ret) {
            cerr << "Error closing ConstrainedMateFixingManager thread. (error " << ret << ")." << endl ;
            exit(-1);
        }
    }
    // write out all of the remaining reads
    while ( ! waitingReads.empty() ) { // there's something in the queue
        writeRead(remove(waitingReads));
    }
}

