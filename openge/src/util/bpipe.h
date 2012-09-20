#ifndef OPENGE_BPIPE_H
#define OPENGE_BPIPE_H
/*********************************************************************
 *
 * bpipe.cpp: A bpipe script object
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 12 September 2012
 *
 *********************************************************************
 *
 * This file is released under the Virginia Tech Non-Commercial
 * Purpose License. A copy of this license has been provided in
 * the openge/ directory.
 *
 *********************************************************************/

#include <string>

template <typename Iterator> struct BpipeParser;

class BPipe {
public:
    BPipe();
    bool load(const std::string & filename);
    bool check();
    bool execute();
    void print();
protected:
    std::string filename;
    std::string script_text;
    BpipeParser<std::string::const_iterator> * parser;
};

#endif