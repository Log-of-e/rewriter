/*
   Copyright 2017-2019 University of Virginia

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <algorithm>
#include <stdlib.h>
#include <fstream>
#include <irdb-core>
#include <getopt.h>
#include <sys/types.h>
#include <unistd.h>
#include "ss.hpp"

using namespace std;
using namespace IRDB_SDK;
using namespace Stamper;

#define ALLOF(a) begin(a), end(a)

//
// A thanos-enabled driver to "stamp" (xor) return addresses on the stack
//
class StackStampDriver_t : public TransformStep_t
{
	public:

		//
		// required overrride: how to parse your arguments
		//
		int parseArgs(const vector<string> step_args) override
		{
			// 
			// convert to argc/argv format for parsing wth getopts 
			//
			// Notes: 
			//    step_args does not include the program name, so we add one in the argv initialization for getopts
			//    C++ containers are a bit weird about having constants, so we cast the const-ness away with const-cast
			//
			auto argv = vector<char*>({const_cast<char*>("libstack_stamp.so")});
			transform(ALLOF(step_args), back_inserter(argv), [](const string &s) -> char* { return const_cast<char*>(s.c_str()); } );
			const auto argc=argv.size();

			//
			// See the RNG and use it to start with a randomized stamp value, before we parse to see if a 
			// particular stack value is requested.
			//
			srand(getpid()+time(NULL));
			stamp_value=rand();

			// declare getopts values 
			const auto short_opts="s:v?h";
			struct option long_options[] = {
				{"stamp-value", required_argument, 0, 's'},
				{"verbose", no_argument, 0, 'v'},
				{"help", no_argument, 0, 'h'},
				{"usage", no_argument, 0, '?'},
				{0,0,0,0}
			};

			// Parse options for the transform
			while(true) 
			{
				auto c = getopt_long(argc, &argv[0], short_opts, long_options, nullptr);
				if(c == -1) break;
				switch(c) 
				{
					case 's': 
						stamp_value=strtoul(optarg,NULL,0);
						break;
					case 'v': 
						verbose=true;
						break;
					case '?':
					case 'h':
						usage(argv[0]);
						return 1;
						break;
					default:
						break;

				}
			}

			// log our stamp value
			cout<<"Stamp value is set to:"<<hex<<stamp_value<<endl;
			return 0;
		}

		//
		// required override:  how to transform the IR
		//
		int executeStep() override
		{
			// get the file's URL for later logging 
			auto url=getMainFile()->getURL();

			// try to load and transform the file IR.
			try
			{
				// Ask thanos for the IR
				auto firp=getMainFileIR();

				// execute a transform.
				const auto success= StackStamp_t(firp, stamp_value, verbose).execute();

				// return success status
				return success ? 0 : 2; // bash-style, 0=success, 1=warnings, 2=errors
			}
			catch (const DatabaseError_t &dberr)
			{
				// log any database errors
				cerr << program_name << ": Unexpected database error: " << dberr << "file url: " << url << endl;
				return 2; // error
			}
			catch (...)
			{
				// log any other errors
				cerr << program_name << ": Unexpected error file url: " << url << endl;
				return 2; // error
			}

		}

		//
		// required override: what is this step's name?
		//
		string getStepName(void) const override
		{
			return program_name;
		}

	private:
	// data
		const string program_name = string("stack_stamp");   // this programs nam
		bool verbose             = false;                    // use verbose mode?
		StampValue_t stamp_value=-1;                // how should we stamp?

	// methods
		
		// 
		// optional print usage for this program
		//
		void usage(const string& name)
		{
			cerr<<"Usage: "<<name<<endl;
			cerr<<"\t--stamp-value <value>         Set the stamp value that will be used.  "<<endl;
			cerr<<"\t-s <value>                    (as parsed by by strtoul)               "<<endl;
			cerr<<"\t--verbose	                   Verbose mode.                           "<<endl;
			cerr<<"\t-v                                                                    "<<endl;
			cerr<<"--help,--usage,-?,-h            Display this message                    "<<endl;
		}

};


//
// Required interface:  libstack_stamp.so needs to have this factory function so thanos can create the transform step object
// 
extern "C"
shared_ptr<TransformStep_t> getTransformStep(void)
{
        return shared_ptr<TransformStep_t>(new StackStampDriver_t());
}


