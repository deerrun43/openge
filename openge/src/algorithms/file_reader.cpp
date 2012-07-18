/*********************************************************************
 *
 * file_reader.cpp:  Algorithm module that filters a stream of reads.
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 31 May 2012
 *
 *********************************************************************
 *
 * This file is released under the Virginia Tech Non-Commercial 
 * Purpose License. A copy of this license has been provided in 
 * the openge/ directory.
 *
 *********************************************************************
 *
 * File reader algorithm module. Provides an abstracted interface to 
 * convert a BAM or SAM file into a stream of reads for other 
 * algorithm module subclasses.
 *
 *********************************************************************/

#include "file_reader.h"

#include "api/BamMultiReader.h"
#include "../util/sam_reader.h"
using namespace BamTools;
using namespace std;

FileReader::file_format_t FileReader::deduceFileFormat()
{
    file_format_t ret_format = FORMAT_UNKNOWN;
    for(int i = 0; i < filenames.size(); i++) {
        file_format_t this_file_format = FORMAT_UNKNOWN;
        FILE * fp = NULL;
        if(filenames[i] == "stdin")
            fp = stdin;
        else {
            fp = fopen(filenames[i].c_str(), "rb");

            if(!fp) {
                cerr << "Couldn't open file " << filenames[i] << endl;
                return FORMAT_UNKNOWN;
            }
        }

        unsigned char data[2];
        if(2 != fread(data, 1,2, fp)) {
            cerr << "Couldn't read from file " << filenames[i] << endl;
            return FORMAT_UNKNOWN;
        }
        
        if(filenames[i] != "stdin")
            fclose(fp);
        else {
            int ret = ungetc(data[1], fp);
            assert(ret != EOF);
            ret = ungetc(data[0], fp);
            assert(ret != EOF);
        }
        
        if(data[0] == '@')
            this_file_format = FORMAT_SAM;
        else if(data[0] == 31 && data[1] == 139)
            this_file_format = FORMAT_BAM;
        else 
            this_file_format = FORMAT_UNKNOWN;

        if(i == 0)
            ret_format = this_file_format;
        
        //check to make sure for multiple files, they are all same format.
        if(i != 0 && ret_format != this_file_format) {
            cerr << "Error: loading files with different file formats is not supported." << endl;
            exit(-1);
        }
            
    }
    
    return ret_format;
}

int FileReader::runInternal()
{
    ogeNameThread("am_FileReader");

    if(!format_specified)
        format = deduceFileFormat();

    if(format == FORMAT_BAM)
    {
        BamMultiReader reader;
        
        if(!reader.Open(filenames)) {
            cerr << "Error opening BAM files." << endl;
            reader.Close();
            return -1;
        }
        
        header = reader.GetHeader();
        references = reader.GetReferenceData();
        open = true;
        
        BamAlignment * al;
        
        while(true)
        {
            if(load_string_data)
                al = reader.GetNextAlignment();
            else
                al = reader.GetNextAlignmentCore();

            if(!al)
                break;
            
            putOutputAlignment(al);
        }
        
        reader.Close();
    } else if(format == FORMAT_SAM) {
        
        vector<SamReader> readers;
        
        SamHeader first_header;
        
        for(int i = 0; i < filenames.size(); i++)   
            readers.push_back(SamReader());

        // before doing any reading, open the files to
        // verify they are the right format, etc.
        for(int i = 0; i < filenames.size(); i++) {   
            SamReader & reader = readers[i];
            
            if(!reader.Open(filenames[i])) {
                cerr << "Error opening SAM file: " << filenames[i] << endl;
                return -1;
            }

            SamHeader header = reader.GetHeader();
            if( i == 0)
                first_header = header;
            
            // TODO: We can probably find a better way to deal with multiple SAM file headers,
            // but for now we should disallow different headers to avoid issues.
            if(i > 0 && header.ToString() != first_header.ToString())
                cerr << "Warning! SAM input files have different headers." << endl;
        }
        
        this->header = first_header;

        for(vector<SamReader>::iterator i = readers.begin(); i != readers.end(); i++) {
            SamReader & reader = *i;
            open = true;

            BamAlignment * al = NULL;
            while(true)
            {
                al = reader.GetNextAlignment();
                
                if(NULL == al)
                    break;
                
                putOutputAlignment(al);
            }

            reader.Close();
        }
    } else {
        cerr << "FileReader couldn't detect file format. Aborting." << endl;
        exit(-1);
        return -1;
    }

    return 0;
}

void FileReader::addFile(std::string filename) 
{
    filenames.push_back(filename); 
}

void FileReader::addFiles(std::vector<std::string> filenames) 
{
    this->filenames.insert(this->filenames.end(), filenames.begin(), filenames.end()); 
}