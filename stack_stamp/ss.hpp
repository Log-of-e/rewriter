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

#ifndef _LIBTRANSFORM_KILL_DEADS_H
#define _LIBTRANSFORM_KILL_DEADS_H

#include <irdb-core>
#include <irdb-transform>
#include <memory>

// 
// using a namespace for code readability
//
namespace Stamper
{
	// std and IRDB namespaces needed
	using namespace std;
	using namespace IRDB_SDK;

	// a type for the stame values
	using StampValue_t = unsigned int;

	// 
	// a class to transform an IR by stamping (xoring) return addresses
	//
	class StackStamp_t : public Transform_t
	{
		public:
			StackStamp_t(FileIR_t *p_variantIR, StampValue_t sv, bool p_verbose);
			bool execute();

		private: 
		// methods

			// determine if we can stamp the given function
			bool can_stamp(Function_t* f);
		
			// stamp a function
			void stamp(Function_t* f);

			// update the function's EH info to reflect the stamp
			void eh_update(Function_t* f);

			// after all eh-pgm re-use has happened, clean stuff up
			void cleanup_eh_pgms();

			// stamp an instruction in a function
			Instruction_t* stamp(Function_t* f, Instruction_t* i);

			// 
			// get the stamp value for a function -- this is for future expansion
			// if we want different stamp values per function.  Right now it
			// just returns the global stamp value
			// 
			StampValue_t get_stamp(Function_t* f);

		// types
			
			// 
			// Stack stamping changes the return address representation.  For
			// programs that use stack unwinding or exception handling, we need 
			// to update the unwind info.  
			//
			// The IRDB FileIR representation shares unwind information between
			// instructions for memory efficiency purposes.  
			//
			// Since we will be editing the IR's unwind info, we have to be cautious
			// about not using excess memory or this transform may build an IR that
			// requires excessive memory for large programs.  (This has ben observed
			// for SPEC's WRF program).
			// 
			// The class below is a cache for EH programs (related to stack unwinding)
			// so we can re-use newly created unwind structures for other instructions.
			//
			struct EhProgramPlaceHolder_t
			{
				// 
				// these are the fields in an EH program
				// we willb e changing only the FDE program, but need the others
				// to decide on equality of EH programs. 
				//
				uint8_t caf;                    // code alignment factor
				int8_t daf;                     // data alignment factor
				int8_t rr;                      // return register
				uint8_t ptrsize;                // pointer size
				EhProgramListing_t cie_program; // the DWARF program in the CIE
				EhProgramListing_t fde_program; // the DWARF program in the FDE
				RelocationSet_t relocs;         // any relocations for the EH program

				// getters
				EhProgramListing_t& getCIEProgram() { return cie_program; }
				EhProgramListing_t& getFDEProgram() { return fde_program; }
				uint8_t getCodeAlignmentFactor() const { return caf; }
				int8_t getDataAlignmentFactor() const { return daf; }
				int8_t getReturnRegNumber() const { return rr; }
				uint8_t getPointerSize() const { return ptrsize; }

				// construct a "placeholder" from the real thing.
				EhProgramPlaceHolder_t(const IRDB_SDK::EhProgram_t* orig)
					:
					caf(orig->getCodeAlignmentFactor()),
					daf(orig->getDataAlignmentFactor()),
					rr(orig->getReturnRegNumber()),
					ptrsize(orig->getPointerSize()),
					cie_program(orig->getCIEProgram()),
					fde_program(orig->getFDEProgram()),
					relocs(orig->getRelocations())
				{
				}

			};

		// data 
			StampValue_t m_stamp_value    = (StampValue_t)0; // how to stamp, for now this value is shared across all functions in the IR
			bool m_verbose                = false;           // how verbose to be

			// a "cache" for EH programs (related to stack unwinding) so we can re-use newly created EH programs
			map<EhProgramPlaceHolder_t, EhProgram_t*> all_eh_pgms;

		// stats 
			int m_instructions_added        = 0;               // how many instructions were added
			int m_functions_transformed     = 0;               // how many functions were transformed
			int m_functions_not_transformed = 0;               // how many functions were skipped

		// friends
			friend bool operator<(const EhProgramPlaceHolder_t &a, const EhProgramPlaceHolder_t& b) ;
	};

	// 
	// this is how we compare "Placeholders" for eh programs.  Useful for std::containers
	//
	// notes:
	//    Put the operator in the namespace for ADL (http://en.wikipedia.org/wiki/Argument-dependent_lookup)
	//
	bool operator<(const StackStamp_t::EhProgramPlaceHolder_t &a, const StackStamp_t::EhProgramPlaceHolder_t& b);
}
#endif
