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

#include <assert.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "ss.hpp"

using namespace std;
using namespace IRDB_SDK;
using namespace Stamper;

// 
// ALLOF is a really useful macro for calling methods from algorithm when you want to work on an entire 
// container.  This macro cannot be made a template function since it can not follow the the C++ language 
// standards.  Thus, it is not recommended to put it in a re-usable header file.
// 
// How it works:  ALLOF(a) returns a comma-separated list of two things -- the begin() and end() of its argument.  
// When used inside an argument list to a function/method call, it thus expands as two arguments.  Calls to the 
// C++ standard template library functions/methods (which takes a pair of iterators) can be simplified when 
// one intends to operate on the entirety of a container.  For example:
//
// 	auto my_iter = find_if(ALLOF(my_container), my_finder); 
//
// is equivilant to:
//
// 	auto my_iter = find_if(begin(my_container), end(my_container), my_finder); 
//
#define ALLOF(s) begin(s), end(s)

// 
// Useful  method to find relocations of a given type. 
// 
static Relocation_t* findRelocation(Instruction_t* insn, const string& type)
{
	const auto &relocs = insn->getRelocations();
	auto find_it=find_if(ALLOF(relocs), [&](const Relocation_t* reloc)
	                     {
	                     	return (reloc->getType()==type);
	                     });
	return (find_it==end(relocs)) ? *find_it : NULL;
}

// 
// How to create a StackStamp_t object. (i.e., the constructor)
// 
StackStamp_t::StackStamp_t(FileIR_t *p_variantIR, StampValue_t sv, bool p_verbose)
	: 
	Transform_t(p_variantIR),
	m_stamp_value(sv),
	m_verbose(p_verbose)
{
}

// 
// get the stamp value for this function  -- for now, just a constant vaule.  Maybe someday later we stamp differnetly.
// 
StampValue_t StackStamp_t::get_stamp(Function_t* f)
{
	return m_stamp_value;
}

// 
// A method to check whether a function is stampable. 
// 
bool StackStamp_t::can_stamp(Function_t* f)
{
	// skip any functions with an entry 
	if(f->getEntryPoint()==NULL) return false;

	// _start does not have a return address on the stack.
	if(f->getName() == "_start") return false;

	// skip functions that might be a plt stub or are so simple they don't count  
	if(f->getInstructions().size()<=3)  return false; 

	// check to see if there are odd instructions in this function that we don't want to stamp 
	const auto fix_call_fallthrough_string=string("fix_call_fallthrough");
	for(const auto insn :  f->getInstructions())
	{
		// decode the insturction
		const auto di=DecodedInstruction_t::factory(insn);

		// grab several fields for later use.
		const auto target=insn->getTarget();
		const auto icfs=insn->getIBTargets();

		//
		// Check to see if this is a "fixed" call.  A fixed call is an x86 call instruction that's been split into a 
		// push/jmp pair.  The push/jmp pair can be relocated to any address without changing the value pushed on the stack.
		// Fixed calls are not frequently used, but occassionally in x86-32 bit cod they are used when calling a "thunk".
		// Fixed calls may also occur when exception handling is used by the application and exception handling rewriting 
		// is disabled.
		//
		// As you won't likely experience a fixed call, further explaination of fixed calls is beyond the scope of the cookbook.
		//
		const auto reloc=findRelocation(insn,fix_call_fallthrough_string);

		// stamp all returns
		if(di->isReturn())
		{
			// returns are OK
		}
		else if(di->isCall() || reloc!=NULL)
		{
			// calls are OK 
			// do not stamp on calls (fixed or otherwise)
		}
		// else if it has a target that exits the function, it's likely a tail call (as a jmp insn)
		else if(target && target->getFunction() != f)
		{
			// generally tails calls are OK, but
			// conditional tail calls?  I just don't want 
			// figuring out how to instrument them.
			if(insn->getFallthrough()!=NULL)
			{
				// log this anomaly.
				cout << "Skipping instrumentation of " << f->getName() << " because of cond branch exit.  Insn is: " << insn->getDisassembly() << endl;
				return false;
			}
		}
		// sanity check indirect cbranches 
		else if(di->isUnconditionalBranch()   && icfs)
		{
			// x86 doesn't have any indirect branches with a fallthrough
			assert(!insn->getFallthrough());

			// I'd be confused if there were an IB with no possible targets.
			// What would this mean?  stop so I can figure it out if it ever happens.
			assert(icfs->size() != 0);


			// like a conditional branch that might leave and might stay,
			// I don't want to instrument IBs that might leave and might stay.
			// So, let's check for those

			// find a target that leaves
			const auto leaver=find_if(ALLOF(*icfs), [&](Instruction_t* target){
			                          return target->getFunction()!=f;
			                          });

			// find a target that stays
			const auto stayer=find_if(ALLOF(*icfs), [&](Instruction_t* target){
			                          return target->getFunction()==f;
			                          });
			
			// calculate booleans about if something might leave
			const auto might_leave=leaver!=icfs->end();
			const auto might_stay=stayer!=icfs->end();

			// if it might not leave, it definitely stays.
			const auto definitely_stays=!might_leave;
			const auto definitely_leaves=!might_stay;

			// if it definitely leaves or stay, go ahead and instrument.
			if(definitely_leaves || definitely_stays)
			{
				// empty 
			}
			else if (might_leave || might_stay)
			{
				// otherwise, it might leave and might stay, so we skip instrumentation.
				return false;
			}
			else
				// this would assert that I wrote the above logic correctly.
				assert(0);
		}
	};

	// we passed all sanity checks, so go ahead and stamp this function
	return true;
}

// 
// How to stamp an individual instruction.
// 
Instruction_t* StackStamp_t::stamp(Function_t* f, Instruction_t* i)
{
	assert(f && i);

	stringstream assembly; // stringstream has no copy constructor, cannot use auto style decls.
	const auto spreg= getFileIR()->getArchitectureBitWidth()==64 ?  string("rsp") : string("esp");

	assembly << " xor dword [" << spreg << "], 0x" << hex << get_stamp(f);

	// 
	// Note about insertAsmBefore:  the old ('after') instruction gets copied to a new Instruction_t, and then the 
	// old Instruction_t gets overwritten with the new assembly as the 'before' instruction.
	// 
	// Thus, after the call:
	//    1) variable 'i' represents 'before'.
	//    2) return value from insertAssembly before represents 'after' 
	// 
	// This can be counterintuitive, but the alternatives are worse.
	// In this example, we do not need the pointer to the newly created instruction (which holds the old assembly),
	// so we simply cast to void 
	// 
	(void)insertAssemblyBefore(i, assembly.str());

	// logging
	if (m_verbose)
	{
		cout << "\tAdding: " << assembly.str() << " before : " << hex<<i->getBaseID()<<":"<<i->getDisassembly() 
		     << "@0x"<<i->getAddress()->getVirtualOffset()<<endl;
	}

	// update stats
	m_instructions_added++;

	return i;
}

// 
// How to update the exception handling (EH) info for a function after we have stamped it.
// 
void StackStamp_t::eh_update(Function_t* f)
{

	// create a new EH program dwarf instruction, that is:
	//	r16= (*(cfa-8)) ^ stamp_value 
	//
	//	the instruction for that is:
	//
	//	DW_CFA_val_expression r16 *(cfa-8) ^ stamp_value.
	//
	//	but the expression is encoded as in prefix notation, which is more like:
	//
	//	push CFA
	//	push 8
	//	minus
	//	deref
	//	push stamp_value
	//	xor
	//
	//	we build it up in multiple parts, the constant prefix, the push of the stamp value, and the constant suffix. 
	//
	const auto stamp_value=(uint64_t)(get_stamp(f));
	const auto dwarf_instruction_prefix=
		getFileIR()->getArchitectureBitWidth()==64 
			? 
				(string)
				{
				0x16, 0x10, 0x0D, /* DW_CFA_val_expression 0x10 (r16 aka RIP) length of prefix expression=13 */\
				0x38 /* DW_OP_lit8 */, \
				0x1c /* DW_op_minus */, \
				0x06 /* DW_OP_deref */
				}
			:
				(string)
				{
				0x16, 0x08, 0x09, /* DW_CFA_val_expression 0x08 (r8 aka EIP) length of prefix expression=9 */\
				0x34 /* DW_OP_lit4 */, \
				0x1c /* DW_op_minus */, \
				0x06 /* DW_OP_deref */
				}
			;
	const auto byte_width=getFileIR()->getArchitectureBitWidth()/8;

	// sanity
	assert(byte_width == 4 || byte_width==8);
	assert((size_t)byte_width<=(size_t)sizeof(stamp_value));

	// this statement may have an endianness issue if the host and the target have different endians.  
	const auto dwarf_instruction_stamp_value=string(reinterpret_cast<const char*>(&stamp_value),byte_width);
	const auto dwarf_addr_insn = ((string){ 0x03 /* DW_OP_addr */ })+dwarf_instruction_stamp_value; 
	const auto dwarf_instruction_suffix=(string){ 0x27 /* DW_OP_xor */};
	const auto dwarf_instruction=(EhProgramInstruction_t)(dwarf_instruction_prefix+dwarf_addr_insn+dwarf_instruction_suffix);

	// 
	// Now add this dwarf unwind instruction to the beginning of the dwarf unwind 
	// info for every machine instruction in the funtion. 
	//
	// We need to do this carefully because creating a new EhProgram for every instruction in a large 
	// program would result in using _lots_ of memory.  Thus, it's necessary to be careful to share EH programs.
	//
	// The input program may end up sharing differently than the output program, so it's necessary to construct
	// a "cache" of EHPrograms to determine how to re-use appropriately.  The cache is organized as a hashtable.
	//
	// The cache starts empty, and we add to do it every time we need a new program.  If we calculate that an 
	// instruciton's new EH program has already been seen, we can re-use the EH program from the cache.
	//
	for(auto insn : f->getInstructions()) 
	{
		// get the old program 
		const auto eh_pgm=insn->getEhProgram();
		// if it didn't have unwind info, don't update anything 
		if(eh_pgm==NULL) continue;

		// 
		// Create a new EH program "placeholder". The placeholder is the "key" in the cache.
		//
		auto nep=EhProgramPlaceHolder_t(eh_pgm);
		auto &fde_pgm=nep.getFDEProgram();

		// Insert the new instructoin into the FDE program. 
		// maybe it'd be better to insert into the CIE program since we are doing the same stamp value for every function?
		// Eliding that for now, as we may want future extensibility.
		//
		fde_pgm.insert(fde_pgm.begin(), dwarf_instruction);

		// now look for this key in our cache 
		const auto reuse_it=all_eh_pgms.find(nep);

		// not found 
		if(reuse_it==all_eh_pgms.end())
		{
			// so we have to create a new EH program from our placeholder for this instrution.
			auto tmp_pgm=getFileIR()->addEhProgram(insn, nep.caf, nep.daf, nep.rr, nep.ptrsize, nep.cie_program, nep.fde_program);

			// and apply the right relocs from the input EH Program. 
			tmp_pgm->setRelocations(nep.relocs);

			// and finally record this new "value" into our cache/hashtable.
			all_eh_pgms[nep]=tmp_pgm;
		}
		else 
		{
			// We found that we've already created this EH program. 
			// So, just share it for this instruction.
			insn->setEhProgram(reuse_it->second);
		}
	};
}

// 
// A routine to cleanup unused EH programs.
//
void StackStamp_t::cleanup_eh_pgms()
{
	// grab a copy of the EH programs just to print the size.
	const auto &old_eh_pgms=getFileIR()->getAllEhPrograms();
	cout<<"# ATTRIBUTE Stack_Stamping::before_transform_exception_handler_programs="<<dec<<old_eh_pgms.size()<<endl;

	// recalculate the new EH programs.
	auto new_eh_pgms=EhProgramSet_t();
	for(auto insn :  getFileIR()->getInstructions())
	{
		const auto eh_pgm=insn->getEhProgram();
		if(eh_pgm!=NULL)
			// found on!
			new_eh_pgms.insert(eh_pgm);
	};

	// now record in the IR that this is the set of EH programs.
	getFileIR()->setAllEhPrograms(new_eh_pgms);

	cout<<"# ATTRIBUTE Stack_Stamping::after_transform_exception_handler_programs="<<dec<<all_eh_pgms.size()<<endl;
	cout<<"# ATTRIBUTE Stack_Stamping::total_instructions="<<dec<<getFileIR()->getInstructions().size()<<endl;
}

// 
// How to stamp an individual function 
//
void StackStamp_t::stamp(Function_t* f)
{
	// preconditions: F is a function from the IR.
	assert(f);

	// check to see if we can stamp the function 
	if(!can_stamp(f))
	{
		// No, record stats.
		cout<<"Skipping "<<dec<<m_functions_transformed<<": "<<f->getName()<<endl;
		m_functions_not_transformed++;

		// and exit.
		return;
	}

	// sanity check can_stamp 
	assert(f->getEntryPoint());

	// Yes, we can stamp.  Do log/stats.
	cout<<"Doing "<<dec<<m_functions_transformed<<": "<<f->getName()<<endl;
	m_functions_transformed++;

	const auto fix_call_fallthrough_string=string("fix_call_fallthrough");

	// Try to stamp each instruction 
	// Correctness note:  the insertAssembly family of functions modifies f->getInstructions().
	// If you try to iterate a container while modifying it, C++ gets very unhappy unless you are careful.
	// Thus, we iterate on a copy of the set.
	const auto old_f_insns=f->getInstructions();
	for(auto insn :  old_f_insns)
	{
		// get some fields of the instruction to work on, including decoding it.
		const auto di=DecodedInstruction_t::factory(insn);
		const auto target=insn->getTarget();
		const auto reloc=findRelocation(insn,fix_call_fallthrough_string);
		const auto icfs=insn->getIBTargets();

		// stamp all returns
		if(di->isReturn())
		{
			if(m_verbose) cout<<"Stamping return"<<endl;
			stamp(f,insn);
		}
		// check for calls specially.
		else if(di->isCall() || reloc!=NULL)
		{
			// do nothing
			// do not stamp on calls (fixed or otherwise)
		}
		// else if it has a target that exits the function, it's likely a tail call (as a jmp insn)
		else if(target && target->getFunction() != f)
		{
			// Conditional jump that leaves the function -- detected in can_stamp as not stampable.
			// not handled yet as we have to instrument oddly.
			assert(!insn->getFallthrough());

			if(m_verbose) cout<<"Stamping with target!=function"<<endl;
			stamp(f,insn);
		} 
		else if(di->isUnconditionalBranch() && icfs)
		{
			// jump with IB targets are likely switches.
			assert(!insn->getFallthrough());

			// This is similar to the logic in can_stamp.  We should isolate into a function/method that
			// avoids the code duplication.  For now, though....

			// make sure there are targets
			assert(icfs->size() != 0);

			// find a target that leaves
			auto leaver=find_if(ALLOF(*icfs), [&](Instruction_t* target){
			                    return target->getFunction()!=f;
			                    });

			// find a target that stays
			auto stayer=find_if(ALLOF(*icfs), [&](Instruction_t* target){
			                    return target->getFunction()==f;
			                    });
			
			// calculate booleans about if something might leave
			bool might_leave=leaver!=icfs->end();
			bool might_stay=stayer!=icfs->end();

			// if it might not leave, it definitely stays.
			bool definitely_leaves=!might_stay;

			// an indirect jump at a function entry needs a stamp	
			if(insn==f->getEntryPoint())
			{
				if(m_verbose) cout << "Stamping IB at entry of function" << endl; 
				stamp(f,insn);
			}
			// stamp if we definitely are leaving this function.
			// or if we're not sure we stay (aka, we might_leave) and we're not complete) 
			// 	this is probably a tail jump that leaves the func, or a plt entry
			else if(definitely_leaves || (might_leave && !icfs->isComplete()))
			{
				if(m_verbose && definitely_leaves ) cout << "Stamping IB because definitely_leaves " << endl; 
				else if(m_verbose) cout << "Stamping IB because might_leave && icfs->isComplete() " << endl; 
			 	stamp(f,insn);
			}
		}
	};

	// do not forget to stamp the entry.
	stamp(f,f->getEntryPoint());

	// Look for any instructions in the function that reference the entry point.
	// Case 1: Those instructions might be recursive calls.  
	// Case 2: The prologue of the function may be empty and the start of a loop.  
	// Try to distinguish between these cases and decide which instructions should
	// jump to the xor and which ones should skip it.
	for(auto insn :  old_f_insns)
	{
		if(insn->getTarget()==f->getEntryPoint())
		{
			const auto di=DecodedInstruction_t::factory(insn);
			// calls should skip it.
			if(!di->isCall())
			{
				cout << "Updating instruction " << hex << insn->getBaseID() << ":" << insn->getDisassembly() << " to skip stamp." << endl;
				insn->setTarget(f->getEntryPoint()->getFallthrough());
			}
		}
	};

	// update the eh frame info.
	eh_update(f);		
}

// 
// How to stamp an entire IR
// 
bool StackStamp_t::execute()
{
	// A sorter that sorts by names without being confused by two funcs with the same name.  
	// This is useful for deterministic debugging.
	struct nameSorter
	{
		bool operator()( const Function_t* lhs, const Function_t* rhs ) const 
		{
			// 
			// Use tie here so we sort by names first, but then by
			// pointer value in the event of two functions with the same name
			// 
			return tie(lhs->getName(), lhs ) < tie(rhs->getName(), rhs);
		}
	};

	// Determine how many functions to stamp.  This would be better done with 
	// a command line option, but someone was lazy...
	const auto ss_max_do_transform = getenv("SS_MAX_DO_TRANSFORM");

	// let's sort the functions so the order of xform is deterministic.
	const auto sorted_funcs = set<Function_t*, nameSorter> (ALLOF(getFileIR()->getFunctions()));

	// try to stamp functions one at a time, in the sorted order
	for(auto func :  sorted_funcs)
	{
		// check to see if we've transformed everything we want already.
		if (ss_max_do_transform && m_functions_transformed > atoi(ss_max_do_transform))
		{
			continue;
		}

		// otherwise, stamp the function.	
		stamp(func);
	};

	// do cleanup on the EH programs after we've likely made many of them useless.
	cleanup_eh_pgms();

	// calculate and output stats 
	const auto pct_transformed=((double)m_functions_transformed/(double)((m_functions_transformed+m_functions_not_transformed)))*100.00;
	const auto pct_not_transformed=((double)m_functions_not_transformed/(double)(m_functions_transformed+m_functions_not_transformed))*100.00;

	cout << "# ATTRIBUTE ASSURANCE_Stack_Stamping::Instructions_added="        << dec << m_instructions_added                                << endl;
	cout << "# ATTRIBUTE ASSURANCE_Stack_Stamping::Total_number_of_functions=" << dec << m_functions_transformed+m_functions_not_transformed << endl;
	cout << "# ATTRIBUTE ASSURANCE_Stack_Stamping::Functions_Transformed="     << dec << m_functions_transformed                             << endl;
	cout << "# ATTRIBUTE ASSURANCE_Stack_Stamping::Functions_Not_Transformed=" << dec << m_functions_not_transformed                         << endl;

	cout << "# ATTRIBUTE ASSURANCE_Stack_Stamping::Percent_Functions_Transformed="     << fixed << setprecision(1) <<  pct_transformed      << "%" << endl;
	cout << "# ATTRIBUTE ASSURANCE_Stack_Stamping::Percent_Functions_Not_Transformed=" << fixed << setprecision(1) <<  pct_not_transformed  << "%" << endl;

	// used in testing harness to verify that the stats are correct.
	assert(getenv("SELF_VALIDATE")==nullptr || m_instructions_added    > 10);
	assert(getenv("SELF_VALIDATE")==nullptr || pct_transformed         > 20);   // can be kind of low for small files
	assert(getenv("SELF_VALIDATE")==nullptr || m_functions_transformed > 5 );

	return m_functions_transformed > 0;  // true means success
}

// 
// How to compare our EH program placeholders.  
// Would this be better done with an explicitly named sorter?
// Possibly it would be more clear what our intent is, but c'est la vie.
// 
bool Stamper::operator<(const StackStamp_t::EhProgramPlaceHolder_t &a, const StackStamp_t::EhProgramPlaceHolder_t& b)
{
	return tie( a.caf, a.daf, a.rr, a.ptrsize, a.cie_program, a.fde_program, a.relocs ) <
	       tie( b.caf, b.daf, b.rr, b.ptrsize, b.cie_program, b.fde_program, b.relocs ) ;
}
