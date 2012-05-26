#include "read_sorter.h"
#include "../util/thread_pool.h"
#include <cassert>
using namespace std;

// ***************************************************************************
// bamtools_mergesort.cpp (c) 2010 Derek Barnett, Erik Garrison, Lee C. Baker
// Marth Lab, Department of Biology, Boston College
// ---------------------------------------------------------------------------
// Last modified: 6 April 2012
// ---------------------------------------------------------------------------
// Merges multiple BAM files into one sorted BAM file
//
// This file contains the bulk of SortTool and MergeTool, running in separate
// threads and paired together with a FIFO. To merge and sort files, the following steps
// are taken:
// # The merged file is read from the FIFO, split into chunks, and passed to worker threads. (Thread #2)
// # Worker threads compress (optional) and then write these sorted chunks to disk. (Thread #3,4...N)
// # The sorted chunks on disk are combined into one final sorted file. (Thread #2)
//
// ***************************************************************************

#include <api/algorithms/Sort.h>

#include <api/SamConstants.h>
#include <api/BamParallelismSettings.h>
#include <api/BamReader.h>
#include <api/BamWriter.h>

#include "mark_duplicates.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <pthread.h>

using namespace BamTools;
using namespace BamTools::Algorithms;

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <iostream>
using namespace std;

const unsigned int SORT_DEFAULT_MAX_BUFFER_COUNT  = 500000;  // max numberOfAlignments for buffer
const unsigned int SORT_DEFAULT_MAX_BUFFER_MEMORY = 1024;    // Mb
const unsigned int MERGESORT_MIN_SORT_SIZE = 30000;    //don't parallelize sort jobs smaller than this many alignments

const char * ALIGNMENT_QUEUE_NAME = "mergesort_align_queue";

//actual run creates threads for other 'run'commands
bool ReadSorter::Run(void)
{
    // options
    if(!isNothreads()) {
        thread_pool = new ThreadPool();
        sort_thread_pool = new ThreadPool();
    } else if(isVerbose())
        cerr << "Thread pool use disabled." << endl;
    
    m_header = getHeader();
    m_header.SortOrder = ( sort_order == SORT_NAME
                          ? Constants::SAM_HD_SORTORDER_QUERYNAME
                          : Constants::SAM_HD_SORTORDER_COORDINATE );

    RunSort();

    if(!isNothreads()) {
        delete thread_pool;
        delete sort_thread_pool;
    }
    
    return sort_retval && merge_retval;
}

// generates mutiple sorted temp BAM files from single unsorted BAM file
bool ReadSorter::GenerateSortedRuns(void) {
    
    if(isVerbose())
        cerr << "Generating sorted temp files...";

    // get basic data that will be shared by all temp/output files
    m_header.SortOrder = ( sort_order == SORT_NAME
                          ? Constants::SAM_HD_SORTORDER_QUERYNAME
                          : Constants::SAM_HD_SORTORDER_COORDINATE );
    
    // set up alignments buffer
    vector<BamAlignment *> * buffer = new vector<BamAlignment *>();
    buffer->reserve( (size_t)(alignments_per_tempfile*1.1) );
    bool bufferFull = false;
    BamAlignment * al = NULL;
    
    // if sorting by name, we need to generate full char data
    // so can't use GetNextAlignmentCore()
    if ( sort_order == SORT_NAME ) {
        
        // iterate through file
        while (true) {
            al = getInputAlignment();
            if(!al)
                break;
            
            // check buffer's usage
            bufferFull = ( buffer->size() >= alignments_per_tempfile );
            
            // store alignments until buffer is "full"
            if ( !bufferFull )
                buffer->push_back(al);
            
            // if buffer is "full"
            else {
                // so create a sorted temp file with current buffer contents
                // then push "al" into fresh buffer
                CreateSortedTempFile(buffer);
                buffer = new vector<BamAlignment *>();
                buffer->reserve( (size_t)(alignments_per_tempfile*1.1) );
                buffer->push_back(al);
            }
        }
    }
    
    // sorting by position, can take advantage of GNACore() speedup
    else {
        // iterate through file
        while (true) {
            al = getInputAlignment();
            if(!al)
                break;
            
            // check buffer's usage
            bufferFull = ( buffer->size() >= alignments_per_tempfile );
            
            // store alignments until buffer is "full"
            if ( !bufferFull )
                buffer->push_back(al);
            
            // if buffer is "full"
            else {
                // create a sorted temp file with current buffer contents
                // then push "al" into fresh buffer
                CreateSortedTempFile(buffer);
                buffer = new vector<BamAlignment *>();
                buffer->push_back(al);
                if(verbose)
                    cerr << "." ;
            }
        }
    }
    
    // handle any leftover buffer contents
    if ( !buffer->empty() )
        CreateSortedTempFile(buffer);
    
    if(isVerbose())
        cerr << "waiting for files to be compressed / written...";
    
    //wait for all temp files to be created in other threads
    if(!isNothreads())
        thread_pool->waitForJobCompletion();
    
    if(isVerbose())
        cerr << "done." << endl;
    
    return true;
}

ReadSorter::TempFileWriteJob::TempFileWriteJob(ReadSorter * tool, vector<BamAlignment *> * buffer, string filename) :
filename(filename), buffer(buffer), tool(tool)
{
}

void ReadSorter::TempFileWriteJob::runJob()
{
#ifdef __linux__
    prctl(PR_SET_NAME,"bt_temp_sort",0,0,0);
#endif
    // do sorting
    tool->SortBuffer(*buffer);
    
#ifdef __linux__
    prctl(PR_SET_NAME,"bt_temp_write",0,0,0);
#endif
    // as noted in the comments of the original file, success is never
    // used so we never return it.
    bool success = tool->WriteTempFile( *buffer, filename );
    if(!success)
        cerr << "Problem writing out temporary file " << filename;
    
#ifdef __linux__
    prctl(PR_SET_NAME,"bt_temp_cleanup",0,0,0);
#endif
    for(size_t i = 0; i < buffer->size(); i++)
        delete (*buffer)[i];
    delete buffer;
}

bool ReadSorter::CreateSortedTempFile(vector<BamAlignment* > * buffer) {
    //make filename
    stringstream tempStr;
    tempStr << m_tempFilenameStub << m_numberOfRuns;
    m_tempFilenames.push_back(tempStr.str());
    
    ++m_numberOfRuns;
    
    bool success = true;
    
    if(isNothreads()) {
        vector<BamAlignment> sort_buffer;
        sort_buffer.reserve(buffer->size());
        for(size_t i = 0; i < buffer->size(); i++)
        {
            BamAlignment * al = buffer->at(i);
            sort_buffer.push_back(*al);
            delete al;
        }
        
        // do sorting
        SortBuffer(sort_buffer);
        
        // write sorted contents to temp file, store success/fail
        success = WriteTempFile( sort_buffer, tempStr.str() );
        delete buffer;
        
    } else {
        TempFileWriteJob * job = new TempFileWriteJob(this, buffer, tempStr.str());
        thread_pool->addJob(job);
    }
    
    // return success/fail of writing to temp file
    // TODO: a failure returned here is not actually caught and handled anywhere
    return success;
}

// merges sorted temp BAM files into single sorted output BAM file
bool ReadSorter::MergeSortedRuns(void) {
    if(verbose)
        cerr << "Combining temp files for final output...";
    
    // open up multi reader for all of our temp files
    BamMultiReader multiReader;
    if ( !multiReader.Open(m_tempFilenames) ) {
        cerr << "mergesort ERROR: could not open BamMultiReader for merging temp files... Aborting."
        << endl;
        return false;
    }

    // while data available in temp files
    BamAlignment * al;
    int count = 0;
    while (NULL != (al = multiReader.GetNextAlignmentCore()) ) {
        putOutputAlignment(al);
        if(count++ % 1000000 == 0 && verbose)
            cerr << ".";  //progress indicator every 1M alignments written.
    }
    
    // close files
    multiReader.Close();
    if(verbose)
        cerr << "done." << endl << "Clearing " << m_tempFilenames.size() << " temp files...";
    
    // delete all temp files
    vector<string>::const_iterator tempIter = m_tempFilenames.begin();
    vector<string>::const_iterator tempEnd  = m_tempFilenames.end();
    for ( ; tempIter != tempEnd; ++tempIter ) {
        const string& tempFilename = (*tempIter);
        remove(tempFilename.c_str());
    }

    if(isVerbose())
        cerr << "done." << endl;

    // return success
    return true;
}

bool ReadSorter::RunSort(void) {
    // this does a single pass, chunking up the input file into smaller sorted temp files,
    // then write out using BamMultiReader to handle merging
    
    if ( GenerateSortedRuns() )
        return MergeSortedRuns();
    else
        return false;
}

template<class T> ReadSorter::SortJob<T>::SortJob(typename vector<T>::iterator begin, typename vector<T>::iterator end, pthread_mutex_t & completion_lock, const ReadSorter & implementation)
: begin(begin)
, end(end)
, completion_lock(completion_lock)
, m_implementation(implementation)
{
}

template<class T> void ReadSorter::SortJob<T>::runJob()
{
#ifdef __linux__
    prctl(PR_SET_NAME,"bt_tempfile_sort",0,0,0);
#endif
    if(m_implementation.sort_order == SORT_NAME)
        std::stable_sort( begin, end, Sort::ByName() );
    else
        std::stable_sort( begin, end, Sort::ByPosition() );
    
    pthread_mutex_unlock(&completion_lock);
}

//this function is designed to accept either BamAlignment or BamAlignment* as T
template<class T>
void ReadSorter::SortBuffer(vector<T>& buffer) {
    
    if(isNothreads())
    {
        if(sort_order == SORT_NAME)
            std::stable_sort( buffer.begin(), buffer.end(), Sort::ByName() );
        else
            std::stable_sort( buffer.begin(), buffer.end(), Sort::ByPosition() );
    } else {
        int divisions = buffer.size() / MERGESORT_MIN_SORT_SIZE;
        if(divisions > ThreadPool::availableCores())
            divisions = ThreadPool::availableCores();
        if(divisions < 1)
            divisions = 1;
        
        pthread_mutex_t * locks = new pthread_mutex_t[divisions];
        SortJob<T> ** jobs = new SortJob<T> *[divisions];
        size_t section_length = buffer.size() / divisions;
        
        //start jobs
        for(int ctr = 0; ctr < divisions; ctr++) {
            if(0 != pthread_mutex_init(&locks[ctr], NULL)) {
                perror("Error initializing a sort mutex");
                assert(0);
            }
            
            if(0 != pthread_mutex_lock(&locks[ctr])) {
                perror("Error locking(1) a sort mutex");
                assert(0);
            }
            
            typename vector<T>::iterator begin = buffer.begin() + ctr * section_length;
            typename vector<T>::iterator end = (ctr == divisions - 1) ? buffer.end() : (buffer.begin() + (1+ctr) * section_length);
            
            jobs[ctr] = new SortJob<T>(begin, end, locks[ctr], *this);
            sort_thread_pool->addJob(jobs[ctr]);
        }
        
        if(0 != pthread_mutex_lock(&locks[0])) {
            perror("Error locking(2) a sort mutex");
            assert(0);
        }
        
        //now, rejoin
        for(int ctr = 1; ctr < divisions; ctr++) {
            
            if(0 != pthread_mutex_lock(&locks[ctr])) {
                perror("Error locking(3) a sort mutex");
                assert(0);
            }
            if(0 != pthread_mutex_unlock(&locks[ctr])) {
                perror("Error unlocking(3) a sort mutex");
                assert(0);
            }
            
            if(0 != pthread_mutex_destroy(&locks[ctr])) {
                perror("Error destroying a sort mutex");
                assert(0);
            }
            
            typename vector<T>::iterator midpoint = buffer.begin() + ctr * section_length;
            typename vector<T>::iterator end = (ctr == divisions - 1) ? buffer.end() : (buffer.begin() + (1+ctr) * section_length);
            
            if(sort_order == SORT_NAME)
                std::inplace_merge(buffer.begin(), midpoint, end, Sort::ByName() );
            else
                std::inplace_merge(buffer.begin(), midpoint, end, Sort::ByPosition() );
        }
        
        delete [] jobs;
        delete [] locks;
    }
}

bool ReadSorter::WriteTempFile(const vector<BamAlignment>& buffer,
                                                                     const string& tempFilename)
{
    // open temp file for writing
    BamWriter tempWriter;
    
    if(compress_temp_files)
        tempWriter.SetCompressionMode(BamWriter::Compressed);
    else
        tempWriter.SetCompressionMode(BamWriter::Uncompressed);
    
    if ( !tempWriter.Open(tempFilename, m_header, m_references) ) {
        cerr << "bamtools sort ERROR: could not open " << tempFilename
        << " for writing." << endl;
        return false;
    }
    
    // write data
    vector<BamAlignment>::const_iterator buffIter = buffer.begin();
    vector<BamAlignment>::const_iterator buffEnd  = buffer.end();
    for ( ; buffIter != buffEnd; ++buffIter )  {
        const BamAlignment& al = (*buffIter);
        tempWriter.SaveAlignment(al);
    }
    
    // close temp file & return success
    tempWriter.Close();
    return true;
}

bool ReadSorter::WriteTempFile(const vector<BamAlignment *>& buffer,
                                                                     const string& tempFilename)
{
    // open temp file for writing
    BamWriter tempWriter;
    
    if(compress_temp_files)
        tempWriter.SetCompressionMode(BamWriter::Compressed);
    else
        tempWriter.SetCompressionMode(BamWriter::Uncompressed);
    
    if ( !tempWriter.Open(tempFilename, m_header, m_references) ) {
        cerr << "bamtools sort ERROR: could not open " << tempFilename
        << " for writing." << endl;
        return false;
    }
    
    // write data
    vector<BamAlignment *>::const_iterator buffIter = buffer.begin();
    vector<BamAlignment *>::const_iterator buffEnd  = buffer.end();
    for ( ; buffIter != buffEnd; ++buffIter )  {
        const BamAlignment * al = (*buffIter);
        tempWriter.SaveAlignment(*al);
    }
    
    // close temp file & return success
    tempWriter.Close();
    return true;
}

SamHeader ReadSorter::getHeader()
{
    SamHeader header = source->getHeader();

    header.SortOrder = ( sort_order == SORT_NAME
                          ? Constants::SAM_HD_SORTORDER_QUERYNAME
                          : Constants::SAM_HD_SORTORDER_COORDINATE );
    return header;
}

int ReadSorter::runInternal()
{
    // run MergeSort, return success/fail
    return Run();
}