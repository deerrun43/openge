#ifndef OGE_ALGO_LOCAL_REALIGNMENT_H
#define OGE_ALGO_LOCAL_REALIGNMENT_H

/*********************************************************************
 *
 * local_realignment.cpp:  Local realignment algorithm.
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 31 May 2012
 *
 *********************************************************************
 *
 * This file has been ported from GATK's implementation in Java, and
 * is distributed under the license listed below.
 *
 *********************************************************************/

/*
 * Copyright (c) 2010 The Broad Institute. 
 * Ported to C++ by Lee C. Baker, Virginia Bioinformatics Institute
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

#include "algorithm_module.h"

#include <string>
#include <queue>

#include "../util/fasta_reader.h"

#include "../util/gatk/AlignmentUtils.h"
#include "../util/gatk/GATKFeature.h"
#include "../util/gatk/GenomeLoc.h"
#include "../util/gatk/GenomeLocParser.h"
#include "../util/gatk/ConstrainedMateFixingManager.h"
#include "../util/gatk/BaseUtils.h"
#include "../util/gatk/GATKFeature.h"
#include "../util/gatk/VariantContext.h"
#include "../util/gatk/ReadMetaDataTracker.h"
#include "../util/gatk/SequenceUtil.h"

//should we support outputing SNPS, INDELS and STATS files?
//#define LR_SUPPORT_ADDITIONAL_OUTPUT_FILES


class LocalRealignment : public AlgorithmModule
{
public:
    
    typedef enum {
        /**
         * Uses only indels from a provided ROD of known indels.
         */
        KNOWNS_ONLY,
        /**
         * Additionally uses indels already present in the original alignments of the reads.
         */
        USE_READS,
        /**
         * Additionally uses 'Smith-Waterman' to generate alternate consenses.
         */
        USE_SW
    } ConsensusDeterminationModel_t;
    
    /**
     * Any number of VCF files representing known indels to be used for constructing alternate consenses.
     * Could be e.g. dbSNP and/or official 1000 Genomes indel calls.  Non-indel variants in these files will be ignored.
     */
    //@Input(fullName="knownAlleles", shortName = "known", doc="Input VCF file(s) with known indels", required=false)
    //vector<RodBinding<VariantContext> > known;
    
    std::string reference_filename;
    std::string output_filename;
    std::string intervals_filename;
    
protected:
    /**
     * The interval list output from the RealignerTargetCreator tool using the same bam(s), reference, and known indel file(s).
     */
    //@Input(fullName="targetIntervals", shortName="targetIntervals", doc="intervals file output from RealignerTargetCreator", required=true)
    std::vector<GenomeLoc *> intervalsFile;
    
    /**
     * This term is equivalent to "significance" - i.e. is the improvement significant enough to merit realignment? Note that this number
     * should be adjusted based on your particular data set. For low coverage and/or when looking for indels with low allele frequency,
     * this number should be smaller.
     */
    //@Argument(fullName="LODThresholdForCleaning", shortName="LOD", doc="LOD threshold above which the cleaner will clean", required=false)
    double LOD_THRESHOLD;

    ConstrainedMateFixingManager * manager;
    
public:
    /**
     * We recommend that users run with USE_READS when trying to realign high quality longer read data mapped with a gapped aligner;
     * Smith-Waterman is really only necessary when using an ungapped aligner (e.g. MAQ in the case of single-end read data).
     */
    //@Argument(fullName = "consensusDeterminationModel", shortName = "model", doc = "Determines how to compute the possible alternate consenses", required = false)
    ConsensusDeterminationModel_t consensusModel;
    
    
    // ADVANCED OPTIONS FOLLOW
    
    /**
     * For expert users only!  This is similar to the argument in the RealignerTargetCreator walker. The point here is that the realigner
     * will only proceed with the realignment (even above the given threshold) if it minimizes entropy among the reads (and doesn't simply
     * push the mismatch column to another position). This parameter is just a heuristic and should be adjusted based on your particular data set.
     */
protected:
    //@Argument(fullName="entropyThreshold", shortName="entropy", doc="percentage of mismatches at a locus to be considered having high entropy", required=false)
    double MISMATCH_THRESHOLD;
    
    /**
     * For expert users only!  To minimize memory consumption you can lower this number (but then the tool may skip realignment on regions with too much coverage;
     * and if the number is too low, it may generate errors during realignment). Just make sure to give Java enough memory! 4Gb should be enough with the default value.
     */
    //@Advanced
    //@Argument(fullName="maxReadsInMemory", shortName="maxInMemory", doc="max reads allowed to be kept in memory at a time by the SAMFileWriter", required=false)
    int MAX_RECORDS_IN_MEMORY;
    
    /**
     * For expert users only!
     */
    //@Advanced
    //@Argument(fullName="maxIsizeForMovement", shortName="maxIsize", doc="maximum insert size of read pairs that we attempt to realign", required=false)
    int MAX_ISIZE_FOR_MOVEMENT;
    
    /**
     * For expert users only!
     */
    //@Advanced
    //@Argument(fullName="maxPositionalMoveAllowed", shortName="maxPosMove", doc="maximum positional move in basepairs that a read can be adjusted during realignment", required=false)
    int MAX_POS_MOVE_ALLOWED;
    
    /**
     * For expert users only!  If you need to find the optimal solution regardless of running time, use a higher number.
     */
    //@Advanced
    //@Argument(fullName="maxConsensuses", shortName="maxConsensuses", doc="max alternate consensuses to try (necessary to improve performance in deep coverage)", required=false)
    int MAX_CONSENSUSES;
    
    /**
     * For expert users only!  If you need to find the optimal solution regardless of running time, use a higher number.
     */
    //@Advanced
    //@Argument(fullName="maxReadsForConsensuses", shortName="greedy", doc="max reads used for finding the alternate consensuses (necessary to improve performance in deep coverage)", required=false)
    int MAX_READS_FOR_CONSENSUSES;
    
    /**
     * For expert users only!  If this value is exceeded at a given interval, realignment is not attempted and the reads are passed to the output file(s) as-is.
     * If you need to allow more reads (e.g. with very deep coverage) regardless of memory, use a higher number.
     */
    //@Advanced
    //@Argument(fullName="maxReadsForRealignment", shortName="maxReads", doc="max reads allowed at an interval for realignment", required=false)
    int MAX_READS;
    
    //@Advanced
    //@Argument(fullName="noOriginalAlignmentTags", shortName="noTags", required=false, doc="Don't output the original cigar or alignment start tags for each realigned read in the output bam")
    bool NO_ORIGINAL_ALIGNMENT_TAGS;
    
    // DEBUGGING OPTIONS FOLLOW
#ifdef LR_SUPPORT_ADDITIONAL_OUTPUT_FILES
    
    //@Hidden
    //@Output(fullName="indelsFileForDebugging", shortName="indels", required=false, doc="Output file (text) for the indels found; FOR DEBUGGING PURPOSES ONLY")
    std::string OUT_INDELS;
    bool write_out_indels;
    
    // @Hidden
    //@Output(fullName="statisticsFileForDebugging", shortName="stats", doc="print out statistics (what does or doesn't get cleaned); FOR DEBUGGING PURPOSES ONLY", required=false)
    std::string OUT_STATS;
    bool write_out_stats;
    
    //@Hidden
    //@Output(fullName="SNPsFileForDebugging", shortName="snps", doc="print out whether mismatching columns do or don't get cleaned out; FOR DEBUGGING PURPOSES ONLY", required=false)
    std::string OUT_SNPS;
    bool write_out_snps;
#endif
    
private:
    static std::vector<CigarOp> unclipCigar(const std::vector<CigarOp> & cigar);
    static bool isClipOperator(const CigarOp op);
    static std::vector<CigarOp> reclipCigar(const std::vector<CigarOp> & cigar, OGERead * read);

#pragma mark AlignedRead
    class AlignedRead {
    private:
        OGERead * read;
        const BamSequenceRecords * sequences;
        std::string readBases;
        std::string baseQuals;
        std::vector<CigarOp> newCigar;
        int newStart;
        int mismatchScoreToReference;
        long alignerMismatchScore;
    public:
        static int MAX_POS_MOVE_ALLOWED;
        static int NO_ORIGINAL_ALIGNMENT_TAGS;
        AlignedRead(OGERead * read, const BamSequenceRecords * sequences)
        : read(read)
        , sequences(sequences)
        , newStart(-1)
        , mismatchScoreToReference(0)
        , alignerMismatchScore(0)
        { }
        
        AlignedRead & operator=(const AlignedRead & other) {
            read = other.read;
            sequences = other.sequences;
            readBases = other.readBases;
            baseQuals = other.baseQuals;
            newCigar = other.newCigar;
            newStart = other.newStart;
            mismatchScoreToReference = other.mismatchScoreToReference;
            alignerMismatchScore = other.alignerMismatchScore;
            return *this;
        }
        
        //AlignedRead & operator=(const AlignedRead & a) { assert(0);  return *this;}
        
        OGERead * getRead() const {
            return read;
        }

        int getReadLength() const {
            return readBases.size() != 0 ? readBases.size() : read->getLength();
        }
        
        
        size_t getCigarLength() const {
            const std::vector<CigarOp> & cigar = (newCigar.size() > 0) ? newCigar : read->getCigarData();
            size_t len = 0;
            
            for(std::vector<CigarOp>::const_iterator i = cigar.begin(); i != cigar.end(); i++) {
                switch(i->type)
                {
                    case 'H':
                    case 'S':
                    case 'D':
                        break;
                    case 'I':
                    default:
                        len += i->length;
                        break;
                }
            }
            return len;
        }
        
        std::string getReadBases();
        
        std::string getBaseQualities();
        
    private:
        // pull out the bases that aren't clipped out
        void getUnclippedBases();
        
        // pull out the bases that aren't clipped out
        std::vector<CigarOp> reclipCigar(const std::vector<CigarOp> & cigar)const ;
    public:
        const std::vector<CigarOp> getCigar() const;
        // tentatively sets the new Cigar, but it needs to be confirmed later
        void setCigar(const std::vector<CigarOp> & cigar, bool fixClippedCigar = true);
        void clearCigar() { newCigar.clear(); }

    public:
        void setAlignmentStart(int start);
        int getAlignmentStart() const;
        int getOriginalAlignmentStart() const;        
        bool constizeUpdate();

        void setMismatchScoreToReference(int score);
        int getMismatchScoreToReference() const;
        void setAlignerMismatchScore(long score);
        long getAlignerMismatchScore() const;
    };
    
#pragma mark Consensus
    class Consensus {
    public:
        std::string str;
        std::vector<std::pair<int, int> > readIndexes;
        int positionOnReference;
        int mismatchSum;
        std::vector<CigarOp> cigar;
        
        Consensus(std::string str, std::vector<CigarOp> cigar, int positionOnReference) 
        : str(str)
        , positionOnReference(positionOnReference)
        , mismatchSum(0)
        , cigar(cigar)
        {}

        bool operator==(const Consensus & c) {
            return ( this == &c || str == c.str ) ;
        }
    };
    
    class ConsensusScoreComparator : public std::less<LocalRealignment::Consensus *> {
    public:
        bool operator()(const Consensus * a, const Consensus * b) {
            return a->mismatchSum < b->mismatchSum;
        }
    };
    
#pragma mark ReadBin
    class ReadBin {
    private:
        std::vector<OGERead *> reads;
        std::string reference;
        GenomeLoc * loc;
        GenomeLocParser * loc_parser;
        const BamSequenceRecords * sequences;
    public:
        ReadBin() 
        : loc(NULL)
        , loc_parser(NULL)
        { }
        
        ~ReadBin() {
            clear();
        }
        
        void initialize(GenomeLocParser * loc_parser, const BamSequenceRecords * sequence_dict) {
            this->loc_parser = loc_parser;
            sequences = sequence_dict;
        }
        
        // Return false if we can't process this read bin because the reads are not correctly overlapping.
        // This can happen if e.g. there's a large known indel with no overlapping reads.
        void add(OGERead * read);
        
        std::vector<OGERead *> getReads() { return reads; }
        
        std::string getReference(FastaReader & referenceReader);
        GenomeLoc getLocation() { return *loc; }
        
        int size() { return reads.size(); }
        
        void clear();
    };
    
private:
    // fasta reference reader to supplement the edges of the reference sequence
    FastaReader * referenceReader;
    GenomeLocParser * loc_parser;
    
    // the intervals input by the user
    std::vector<GenomeLoc *>::iterator interval_it;
    
    // the current interval in the list
    bool sawReadInCurrentInterval;
    
    // the reads and known indels that fall into the current interval
    class IntervalData {
    public:
        IntervalData(GenomeLoc & currentInterval)
        : current_interval(currentInterval)
        , current_interval_valid(true)
        , ready_for_flush(false)
        {}
        IntervalData(GenomeLoc * currentInterval)
        : current_interval(currentInterval == NULL ? GenomeLoc("None", 0,0,0) : *currentInterval)
        , current_interval_valid(currentInterval != NULL)
        , ready_for_flush(false)
        { }
        IntervalData()
        : current_interval(GenomeLoc("None",0,0,0))
        , current_interval_valid(false)
        , ready_for_flush(false)
        {}
        ReadBin readsToClean;
        std::vector<OGERead *> readsNotToClean;
        std::vector<VariantContext> knownIndelsToTry;
        std::set<GATKFeature> indelRodsSeen;
        std::set<OGERead *> readsActuallyCleaned;
        const GenomeLoc current_interval;
        bool current_interval_valid;
        bool ready_for_flush;
    };
    
    IntervalData * loading_interval_data;
    
    BamSequenceRecords sequence_dictionary;
    
    static const int MAX_QUAL;
    
    // fraction of mismatches that need to no longer mismatch for a column to be considered cleaned
    static const double MISMATCH_COLUMN_CLEANED_FRACTION;
    
    // reference base padding size
    // TODO -- make this a command-line argument if the need arises
    static const int REFERENCE_PADDING;
    
    // other output files
            
#ifdef LR_SUPPORT_ADDITIONAL_OUTPUT_FILES
    bool outputIndels, output_stats, output_snps;
    std::ofstream indelOutput, statsOutput, snpsOutput;
#endif
    
    GenomeLoc * current_interval;
    
    class Emittable
    {
    public:
        virtual void emit() = 0;
        virtual bool canEmit() = 0;
        virtual ~Emittable();
    };
    class EmittableRead;
    class EmittableReadList;
    class CleanAndEmitReadList;

    std::queue<Emittable *> emit_queue; //queue up reads ready to be emitted so that they are in order, including ReadBins that have been cleaned.
    mutex emit_mutex; // only one thread should emit() at once
    
    void flushEmitQueue() {
        // since multiple threads will call this, we need to ensure taht all thread pool workers
        // don't get stuck waiting for this mutex. It isn't critical for each worker to run this
        // function, and we will call it over and over at the end to ensure everything is flushed.
        // We can give some threads an easy-out.
        if(!emit_mutex.try_lock())
            return;
        
        while(! emit_queue.empty() && emit_queue.front()->canEmit()) {
            emit_queue.front()->emit();
            delete emit_queue.front();
            emit_queue.pop();
        }
        
        emit_mutex.unlock();
    }
            
    void pushToEmitQueue(Emittable * e)
    {
        bool emit_queue_full = true;
        while(emit_queue_full) {
            emit_mutex.lock();
            emit_queue_full = emit_queue.size() > 1000;
            if(!emit_queue_full)
                emit_queue.push(e);
            
            emit_mutex.unlock();
            
            if(emit_queue_full) {
                usleep(20000);
                flushEmitQueue();
            }
        }
    }
    
public:
    void initialize();
    void writeRead(OGERead * read) { putOutputAlignment(read); }

private:
    void emit(IntervalData & interval_data, OGERead * read);
    void emitReadLists(IntervalData & interval_data);
    
public:
    int map_func(OGERead * read, const ReadMetaDataTracker & metaDataTracker);

private:
    bool doNotTryToClean(const OGERead & read);
    
public:
    void onTraversalDone(IntervalData & interval_data, int result);

private:
    void populateKnownIndels(IntervalData & interval_data, const ReadMetaDataTracker & metaDataTracker) ;
    
    static int mismatchQualitySumIgnoreCigar(AlignedRead & aRead, const std::string & refSeq, int refIndex, int quitAboveThisValue);
    
    void clean(IntervalData & interval_data) const;
    void generateAlternateConsensesFromKnownIndels(IntervalData & interval_data, std::set<Consensus *> & altConsensesToPopulate, const int leftmostIndex, const std::string reference) const;
    long determineReadsThatNeedCleaning( std::vector<OGERead *> & reads,
                                        std::vector<OGERead *> & refReadsToPopulate,
                                        std::vector<AlignedRead *> & altReadsToPopulate,
                                        std::vector<AlignedRead *> & altAlignmentsToTest,
                                        std::set<Consensus *> & altConsenses,
                                        int leftmostIndex,
                                        std::string & reference) const;
    void generateAlternateConsensesFromReads( std::vector<AlignedRead> & altAlignmentsToTest,
                                             std::set<Consensus *> & altConsensesToPopulate,
                                             const std::string & reference,
                                             const int leftmostIndex);
    Consensus * createAlternateConsensus(const int indexOnRef, const std::vector<CigarOp> & c, const std::string reference, const std::string readStr) const;
    Consensus * createAlternateConsensus(const int indexOnRef, const std::string & reference, const std::string & indelStr, VariantContext indel) const;
    std::pair<int, int> findBestOffset(const std::string & ref, AlignedRead read, const int leftmostIndex) const;
    bool updateRead(const std::vector<CigarOp> & altCigar, const int altPosOnRef, const int myPosOnAlt, AlignedRead & aRead, const int leftmostIndex) const;
    bool alternateReducesEntropy(const std::vector<AlignedRead *> & reads, const std::string & reference, const int leftmostIndex) const;

protected:
public:
    bool verbose;
    LocalRealignment();
    
    std::string getReferenceFilename() { return reference_filename; }
    void setReferenceFilename(const std::string & filename) { this->reference_filename = filename; }
    std::string getIntervalsFilename() { return intervals_filename; }
    void setIntervalsFilename(const std::string & filename) { this->intervals_filename = filename; }

protected:
    int runInternal();
};

#endif
