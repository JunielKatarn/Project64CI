#include "stdafx.h"

#define CHECKED_BUILD 1

int DelaySlotEffectsCompare ( DWORD PC, DWORD Reg1, DWORD Reg2 );

LoopAnalysis::LoopAnalysis(CCodeBlock * CodeBlock, CCodeSection * Section) :
	m_EnterSection(Section),
	m_BlockInfo(CodeBlock),
	m_PC((DWORD)-1),
	m_NextInstruction(NORMAL),
	m_Test(m_BlockInfo->NextTest()),
	m_TestChanged(0)
{
	memset(&m_Command,0,sizeof(m_Command));
}

LoopAnalysis::~LoopAnalysis()
{
	for (RegisterMap::iterator itr = m_EnterRegisters.begin(); itr != m_EnterRegisters.end(); itr++)
	{
		delete itr->second;
	}
	m_EnterRegisters.clear();

	for (RegisterMap::iterator itr = m_ContinueRegisters.begin(); itr != m_ContinueRegisters.end(); itr++)
	{
		delete itr->second;
	}
	m_ContinueRegisters.clear();

	for (RegisterMap::iterator itr = m_JumpRegisters.begin(); itr != m_JumpRegisters.end(); itr++)
	{
		delete itr->second;
	}
	m_JumpRegisters.clear();
}

bool LoopAnalysis::SetupRegisterForLoop ( void )
{
	if (!m_EnterSection->m_InLoop)
	{
		return false;
	}
	CPU_Message(__FUNCTION__ ": Section ID: %d Test: %X",m_EnterSection->m_SectionID,m_Test);
	do 
	{
		if (!CheckLoopRegisterUsage(m_EnterSection))
		{
			return false;
		}
	} while (m_EnterSection->m_Test != m_Test);

	RegisterMap::iterator itr = m_EnterRegisters.find(m_EnterSection->m_SectionID);
	if (itr == m_EnterRegisters.end())
	{
		return false;
	}
	m_EnterSection->m_RegEnter = *(itr->second);
	return true;
}

bool LoopAnalysis::SetupEnterSection ( CCodeSection * Section )
{
	if (Section->m_ParentSection.empty()) { _Notify->BreakPoint(__FILE__,__LINE__); return true; }

	CPU_Message(__FUNCTION__ ": Block EnterPC: %X Section ID %d Test: %X Section Test: %X CompiledLocation: %X",m_BlockInfo->VAddrEnter(),Section->m_SectionID,m_Test,Section->m_Test, Section->m_CompiledLocation);

	bool bFirstParent = true, bSkipedSection = false;
	CRegInfo RegEnter;
	for (CCodeSection::SECTION_LIST::iterator iter = Section->m_ParentSection.begin(); iter != Section->m_ParentSection.end(); iter++)
	{
		CCodeSection * Parent = *iter;

		CPU_Message(__FUNCTION__ ": Parent Section ID %d Test: %X Section Test: %X CompiledLocation: %X",Parent->m_SectionID,m_Test,Parent->m_Test, Parent->m_CompiledLocation);
		if (Parent->m_Test != m_Test && (m_EnterSection != Section || Parent->m_CompiledLocation == NULL))
		{
			CPU_Message(__FUNCTION__ ": Ignore Parent Section ID %d Test: %X  Section Test: %X CompiledLocation: %X",Parent->m_SectionID,m_Test,Parent->m_Test, Parent->m_CompiledLocation);
			bSkipedSection = true;
			continue;
		}
		RegisterMap::iterator Continue_itr = m_ContinueRegisters.find(Parent->m_SectionID);
		RegisterMap::iterator Jump_itr = m_JumpRegisters.find(Parent->m_SectionID);

		CCodeSection * TargetSection[] = { Parent->m_ContinueSection, Parent->m_JumpSection };
		CRegInfo * JumpRegInfo[] = { 
			Continue_itr == m_ContinueRegisters.end() ? &Parent->m_Cont.RegSet : Continue_itr->second, 
			Jump_itr == m_JumpRegisters.end() ? &Parent->m_Jump.RegSet : Jump_itr->second
		};

		for (int i = 0; i < 2; i++)
		{
			if (TargetSection[i] != Section) { continue; }
			if (bFirstParent)
			{
				bFirstParent = false;
				RegEnter = *JumpRegInfo[i];
			} else {
				if (*JumpRegInfo[i] == RegEnter)
				{
					continue;
				}

				SyncRegState(RegEnter,*JumpRegInfo[i]);
			}
		}
	}

	if (bFirstParent)
	{
		_Notify->BreakPoint(__FILE__,__LINE__);
	}

	RegisterMap::iterator itr = m_EnterRegisters.find(Section->m_SectionID);
	if (itr != m_EnterRegisters.end())
	{
		if (SyncRegState(*(itr->second),RegEnter))
		{
			m_Test = m_BlockInfo->NextTest();
			m_TestChanged += 1;
			if (m_TestChanged > MAX_TESTCHANGED)
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			}
		}
	} else {
		m_EnterRegisters.insert(RegisterMap::value_type(Section->m_SectionID,new CRegInfo(RegEnter)));
	}
	return true;
}

bool LoopAnalysis::CheckLoopRegisterUsage( CCodeSection * Section) 
{
	if (Section == NULL) { return true; }
	if (!Section->m_InLoop) { return true; }

	if (Section->m_Test == m_Test)
	{
		return true;
	}

	if (!SetupEnterSection(Section)) { return false; }
	
	CPU_Message(__FUNCTION__ ": Set Section %d test to %X from %X",Section->m_SectionID,m_Test,Section->m_Test);
	Section->m_Test = m_Test;
	m_PC = Section->m_EnterPC;

	RegisterMap::iterator itr = m_EnterRegisters.find(Section->m_SectionID);
	m_Reg = itr != m_EnterRegisters.end() ? *(itr->second) : Section->m_RegEnter;

	m_NextInstruction = NORMAL;
	DWORD ContinueSectionPC = Section->m_ContinueSection ? Section->m_ContinueSection->m_EnterPC : (DWORD)-1;
	CPU_Message("ContinueSectionPC = %08X",ContinueSectionPC);

	do {
		if (!_MMU->LW_VAddr(m_PC, m_Command.Hex)) 
		{
			_Notify->BreakPoint(__FILE__,__LINE__);
			return false;
		}		
		CPU_Message("  %08X: %s",m_PC,R4300iOpcodeName(m_Command.Hex,m_PC));
		CPU_Message("  %s state: %X value: %X",CRegName::GPR[1],m_Reg.MipsRegState(1),m_Reg.MipsRegLo(1));
		switch (m_Command.op) {
		case R4300i_SPECIAL:
			switch (m_Command.funct) {
			case R4300i_SPECIAL_SLL: SPECIAL_SLL(); break;
			case R4300i_SPECIAL_SRL: SPECIAL_SRL(); break;
			case R4300i_SPECIAL_SRA: SPECIAL_SRA(); break;
			case R4300i_SPECIAL_SLLV: SPECIAL_SLLV(); break; 
			case R4300i_SPECIAL_SRLV: SPECIAL_SRLV(); break;
			case R4300i_SPECIAL_SRAV: SPECIAL_SRAV(); break;
			case R4300i_SPECIAL_JR:	SPECIAL_JR(); break;
			case R4300i_SPECIAL_JALR: SPECIAL_JALR(); break;
			case R4300i_SPECIAL_SYSCALL: SPECIAL_SYSCALL(); break;
			case R4300i_SPECIAL_BREAK: SPECIAL_BREAK(); break;
			case R4300i_SPECIAL_MFHI: SPECIAL_MFHI(); break;
			case R4300i_SPECIAL_MTHI: SPECIAL_MTHI(); break;
			case R4300i_SPECIAL_MFLO: SPECIAL_MFLO(); break; 
			case R4300i_SPECIAL_MTLO: SPECIAL_MTLO(); break; 
			case R4300i_SPECIAL_DSLLV: SPECIAL_DSLLV(); break; 
			case R4300i_SPECIAL_DSRLV: SPECIAL_DSRLV(); break; 
			case R4300i_SPECIAL_DSRAV: SPECIAL_DSRAV(); break; 
			case R4300i_SPECIAL_MULT: break;
			case R4300i_SPECIAL_MULTU: break;
			case R4300i_SPECIAL_DIV: break;
			case R4300i_SPECIAL_DIVU: break;
			case R4300i_SPECIAL_DMULT: break;
			case R4300i_SPECIAL_DMULTU: break;
			case R4300i_SPECIAL_DDIV: break;
			case R4300i_SPECIAL_DDIVU: break;
			case R4300i_SPECIAL_ADD: SPECIAL_ADD(); break;
			case R4300i_SPECIAL_ADDU: SPECIAL_ADDU(); break;
			case R4300i_SPECIAL_SUB: SPECIAL_SUB(); break;
			case R4300i_SPECIAL_SUBU: SPECIAL_SUBU(); break;
			case R4300i_SPECIAL_AND: SPECIAL_AND(); break;
			case R4300i_SPECIAL_OR: SPECIAL_OR(); break; 
			case R4300i_SPECIAL_XOR: SPECIAL_XOR(); break; 
			case R4300i_SPECIAL_NOR: SPECIAL_NOR(); break; 
			case R4300i_SPECIAL_SLT: SPECIAL_SLT(); break; 
			case R4300i_SPECIAL_SLTU: SPECIAL_SLTU(); break; 
			case R4300i_SPECIAL_DADD: SPECIAL_DADD(); break;
			case R4300i_SPECIAL_DADDU: SPECIAL_DADDU(); break;
			case R4300i_SPECIAL_DSUB: SPECIAL_DSUB(); break;
			case R4300i_SPECIAL_DSUBU: SPECIAL_DSUBU(); break;
			case R4300i_SPECIAL_DSLL: SPECIAL_DSLL(); break;
			case R4300i_SPECIAL_DSRL: SPECIAL_DSRL(); break;
			case R4300i_SPECIAL_DSRA: SPECIAL_DSRA(); break;
			case R4300i_SPECIAL_DSLL32: SPECIAL_DSLL32(); break;
			case R4300i_SPECIAL_DSRL32: SPECIAL_DSRL32(); break;
			case R4300i_SPECIAL_DSRA32: SPECIAL_DSRA32(); break;
			default:
				_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
				if (m_Command.Hex == 0x00000001) { break; }
				_Notify->DisplayError("Unhandled R4300i OpCode in FillSectionInfo 5\n%s",
					R4300iOpcodeName(m_Command.Hex,m_PC));
#endif
				m_NextInstruction = END_BLOCK;
				m_PC -= 4;
			}
			break;
		case R4300i_REGIMM:
			switch (m_Command.rt) {
			case R4300i_REGIMM_BLTZ:
			case R4300i_REGIMM_BGEZ:
				m_NextInstruction = DELAY_SLOT;
#ifdef CHECKED_BUILD
				if (Section->m_Cont.TargetPC != m_PC + 8)
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}
				if (Section->m_Jump.TargetPC != m_PC + ((short)m_Command.offset << 2) + 4)
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}
				if (m_PC == Section->m_Jump.TargetPC) 
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
					if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,m_Command.rt)) {
						Section->m_Jump.PermLoop = true;
					}
#endif
				} 
#endif
				break;
			case R4300i_REGIMM_BLTZL:
			case R4300i_REGIMM_BGEZL:
				m_NextInstruction = LIKELY_DELAY_SLOT;
#ifdef CHECKED_BUILD
				if (Section->m_Cont.TargetPC != m_PC + 8)
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}
				if (Section->m_Jump.TargetPC != m_PC + 4)
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}
				/*if (Section->m_Jump.TargetPC != m_PC + ((short)m_Command.offset << 2) + 4)
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}*/
				if (m_PC == m_PC + ((short)m_Command.offset << 2) + 4) 
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
					if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,m_Command.rt)) 
					{
						if (!Section->m_Jump.PermLoop)
						{
							_Notify->BreakPoint(__FILE__,__LINE__);
						}
					}
#endif
				} 
#endif
				break;
			case R4300i_REGIMM_BLTZAL:
				_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
				m_Reg.MipsRegLo(31) = m_PC + 8;
				m_Reg.SetMipsRegState(31,CRegInfo::STATE_CONST_32);
				Section->m_Cont.TargetPC = m_PC + 8;
				Section->m_Jump.TargetPC = m_PC + ((short)m_Command.offset << 2) + 4;
				if (m_PC == Section->m_Jump.TargetPC) { 
					if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,0)) {
						Section->m_Jump.PermLoop = true;
					}
				} 
#endif
				break;
			case R4300i_REGIMM_BGEZAL:
				_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
				m_NextInstruction = DELAY_SLOT;
				if (m_Reg.IsConst(m_Command.rs)) 
				{
					__int64 Value;
					if (m_Reg.Is32Bit(m_Command.rs))
					{
						Value = m_Reg.MipsRegLo_S(m_Command.rs);
					} else {
						Value = m_Reg.cMipsReg_S(m_Command.rs);
					}
					if (Value >= 0) {
						m_Reg.MipsRegLo(31) = m_PC + 8;
							m_Reg.SetMipsRegState(31,CRegInfo::STATE_CONST_32);
						Section->m_Jump.TargetPC = m_PC + ((short)m_Command.offset << 2) + 4;
						if (m_PC == Section->m_Jump.TargetPC) {
							if (!DelaySlotEffectsCompare(m_PC,31,0)) {
								Section->m_Jump.PermLoop = true;
							}
						} 
						break;
					}
				} 

				
				m_Reg.MipsRegLo(31) = m_PC + 8;
				m_Reg.SetMipsRegState(31,CRegInfo::STATE_CONST_32);
				Section->m_Cont.TargetPC = m_PC + 8;
				Section->m_Jump.TargetPC = m_PC + ((short)m_Command.offset << 2) + 4;
				if (m_PC == Section->m_Jump.TargetPC) { 
					if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,0)) {
						Section->m_Jump.PermLoop = true;
					}
				} 
#endif
				break;
			default:
				_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
				if (m_Command.Hex == 0x0407000D) { break; }
				_Notify->DisplayError("Unhandled R4300i OpCode in FillSectionInfo 4\n%s",
					R4300iOpcodeName(m_Command.Hex,m_PC));
				m_NextInstruction = END_BLOCK;
				m_PC -= 4;
#endif
			}
			break;
		case R4300i_JAL: 
			_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
			m_NextInstruction = DELAY_SLOT;
			m_Reg.MipsRegLo(31) = m_PC + 8;
				m_Reg.SetMipsRegState(31,CRegInfo::STATE_CONST_32);
			Section->m_Jump.TargetPC = (m_PC & 0xF0000000) + (m_Command.target << 2);
			if (m_PC == Section->m_Jump.TargetPC) {
				if (!DelaySlotEffectsCompare(m_PC,31,0)) {
					Section->m_Jump.PermLoop = true;
				}
			} 
#endif
			break;
		case R4300i_J: 
			m_NextInstruction = DELAY_SLOT;
#ifdef CHECKED_BUILD
			if (Section->m_Jump.TargetPC != (m_PC & 0xF0000000) + (m_Command.target << 2))
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			}
			if (m_PC == Section->m_Jump.TargetPC && !Section->m_Jump.PermLoop) 
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			} 
#endif
			break;
		case R4300i_BEQ: 
			m_NextInstruction = DELAY_SLOT;
#ifdef CHECKED_BUILD
			if (m_Command.rs != 0 || m_Command.rt != 0)
			{
				if (Section->m_Cont.TargetPC != m_PC + 8 && 
					Section->m_ContinueSection != NULL && 
					Section->m_Cont.TargetPC != (DWORD)-1)
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}
			}  else {
				if (Section->m_Cont.TargetPC != (DWORD)-1)
				{
					//_Notify->BreakPoint(__FILE__,__LINE__);
				}
			}
			if (Section->m_Jump.TargetPC != m_PC + ((short)m_Command.offset << 2) + 4)
			{
				//_Notify->BreakPoint(__FILE__,__LINE__);
			}
			if (m_PC == Section->m_Jump.TargetPC) 
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
				if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,m_Command.rt)) {
					Section->m_Jump.PermLoop = true;
				}
#endif
			} 
#endif
			break;
		case R4300i_BNE: 
		case R4300i_BLEZ: 
		case R4300i_BGTZ: 
			m_NextInstruction = DELAY_SLOT;
#ifdef CHECKED_BUILD
			if (Section->m_Cont.TargetPC != m_PC + 8 && 
				Section->m_ContinueSection != NULL && 
				Section->m_Cont.TargetPC != (DWORD)-1)
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			}
			if (Section->m_Jump.TargetPC != m_PC + ((short)m_Command.offset << 2) + 4)
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			}
			if (m_PC == Section->m_Jump.TargetPC) 
			{
				if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,m_Command.rt) && !Section->m_Jump.PermLoop) 
				{
					_Notify->BreakPoint(__FILE__,__LINE__);
				}
			}
#endif
			break;
		case R4300i_ADDI: 
		case R4300i_ADDIU: 
			if (m_Command.rt == 0) { break; }
			/*if (m_Command.rs == 0) { 
				m_Reg.MipsRegLo(m_Command.rt) = (short)m_Command.immediate;
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
			} else {*/
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			//}
			break;
		case R4300i_SLTI: 
			if (m_Command.rt == 0) { break; }
			if (m_Reg.IsConst(m_Command.rs)) { 
				if (m_Reg.Is64Bit(m_Command.rs)) {
					m_Reg.MipsRegLo(m_Command.rt) = (m_Reg.cMipsReg_S(m_Command.rs) < (_int64)((short)m_Command.immediate))?1:0;
				} else {
					m_Reg.MipsRegLo(m_Command.rt) = (m_Reg.MipsRegLo_S(m_Command.rs) < (int)((short)m_Command.immediate))?1:0;
				}
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
			} else {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			}
			break;
		case R4300i_SLTIU: 
			if (m_Command.rt == 0) { break; }
			if (m_Reg.IsConst(m_Command.rs)) { 
				if (m_Reg.Is64Bit(m_Command.rs)) {
					m_Reg.MipsRegLo(m_Command.rt) = (m_Reg.MipsReg(m_Command.rs) < (unsigned _int64)((short)m_Command.immediate))?1:0;
				} else {
					m_Reg.MipsRegLo(m_Command.rt) = (m_Reg.MipsRegLo(m_Command.rs) < (DWORD)((short)m_Command.immediate))?1:0;
				}
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
			} else {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			}
			break;
		case R4300i_LUI: 
			if (m_Command.rt == 0) { break; }
			m_Reg.MipsRegLo(m_Command.rt) = ((short)m_Command.offset << 16);
			m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
			break;
		case R4300i_ANDI: 
			if (m_Command.rt == 0) { break; }
			if (m_Command.rs == m_Command.rt) 
			{
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);	
			} else if (m_Reg.IsConst(m_Command.rs)) {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
				m_Reg.MipsRegLo(m_Command.rt) = m_Reg.MipsRegLo(m_Command.rs) & m_Command.immediate;
			} else {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			}
			break;
		case R4300i_ORI: 
			if (m_Command.rt == 0) { break; }
			if (m_Command.rs == m_Command.rt) 
			{
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);	
			}
			if (m_Reg.IsConst(m_Command.rs)) {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
				m_Reg.MipsRegLo(m_Command.rt) = m_Reg.MipsRegLo(m_Command.rs) | m_Command.immediate;
			} else {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			}
			break;
		case R4300i_XORI: 
			if (m_Command.rt == 0) { break; }
			if (m_Command.rs == m_Command.rt) 
			{
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);	
			}
			if (m_Reg.IsConst(m_Command.rs)) {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_32);
				m_Reg.MipsRegLo(m_Command.rt) = m_Reg.MipsRegLo(m_Command.rs) ^ m_Command.immediate;
			} else {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			}
			break;
		case R4300i_CP0:
			switch (m_Command.rs) {
			case R4300i_COP0_MF:
				if (m_Command.rt == 0) { break; }
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
				break;
			case R4300i_COP0_MT: break;
			default:
				if ( (m_Command.rs & 0x10 ) != 0 ) {
					switch( m_Command.funct ) {
					case R4300i_COP0_CO_TLBR: break;
					case R4300i_COP0_CO_TLBWI: break;
					case R4300i_COP0_CO_TLBWR: break;
					case R4300i_COP0_CO_TLBP: break;
					case R4300i_COP0_CO_ERET: m_NextInstruction = END_BLOCK; break;
					default:
						_Notify->DisplayError("Unhandled R4300i OpCode in FillSectionInfo\n%s",
							R4300iOpcodeName(m_Command.Hex,m_PC));
						m_NextInstruction = END_BLOCK;
						m_PC -= 4;
					}
				} else {
					_Notify->DisplayError("Unhandled R4300i OpCode in FillSectionInfo 3\n%s",
						R4300iOpcodeName(m_Command.Hex,m_PC));
					m_NextInstruction = END_BLOCK;
					m_PC -= 4;
				}
			}
			break;
		case R4300i_CP1:
			switch (m_Command.fmt) {
			case R4300i_COP1_CF:
			case R4300i_COP1_MF:
			case R4300i_COP1_DMF:
				if (m_Command.rt == 0) { break; }
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
				break;
			case R4300i_COP1_BC:
				switch (m_Command.ft) {
				case R4300i_COP1_BC_BCFL:
				case R4300i_COP1_BC_BCTL:
					m_NextInstruction = LIKELY_DELAY_SLOT;
					_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
					Section->m_Cont.TargetPC = m_PC + 8;
					Section->m_Jump.TargetPC = m_PC + ((short)m_Command.offset << 2) + 4;
					if (m_PC == Section->m_Jump.TargetPC) {
						int EffectDelaySlot;
						OPCODE NewCommand;

						if (!_MMU->LW_VAddr(m_PC + 4, NewCommand.Hex)) {
							_Notify->DisplayError(GS(MSG_FAIL_LOAD_WORD));
							ExitThread(0);
						}
						
						EffectDelaySlot = false;
						if (NewCommand.op == R4300i_CP1) {
							if (NewCommand.fmt == R4300i_COP1_S && (NewCommand.funct & 0x30) == 0x30 ) {
								EffectDelaySlot = true;
							} 
							if (NewCommand.fmt == R4300i_COP1_D && (NewCommand.funct & 0x30) == 0x30 ) {
								EffectDelaySlot = true;
							} 
						}						
						if (!EffectDelaySlot) {
							Section->m_Jump.PermLoop = true;
						}
					} 
#endif
					break;
				case R4300i_COP1_BC_BCF:
				case R4300i_COP1_BC_BCT:
					m_NextInstruction = DELAY_SLOT;
					_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
					Section->m_Cont.TargetPC = m_PC + 8;
					Section->m_Jump.TargetPC = m_PC + ((short)m_Command.offset << 2) + 4;
					if (m_PC == Section->m_Jump.TargetPC) {
						int EffectDelaySlot;
						OPCODE NewCommand;

						if (!_MMU->LW_VAddr(m_PC + 4, NewCommand.Hex)) {
							_Notify->DisplayError(GS(MSG_FAIL_LOAD_WORD));
							ExitThread(0);
						}
						
						EffectDelaySlot = false;
						if (NewCommand.op == R4300i_CP1) {
							if (NewCommand.fmt == R4300i_COP1_S && (NewCommand.funct & 0x30) == 0x30 ) {
								EffectDelaySlot = true;
							} 
							if (NewCommand.fmt == R4300i_COP1_D && (NewCommand.funct & 0x30) == 0x30 ) {
								EffectDelaySlot = true;
							} 
						}						
						if (!EffectDelaySlot) {
							Section->m_Jump.PermLoop = true;
						}
					} 
#endif
					break;
				}
				break;
			case R4300i_COP1_MT: break;
			case R4300i_COP1_DMT: break;
			case R4300i_COP1_CT: break;
			case R4300i_COP1_S: break;
			case R4300i_COP1_D: break;
			case R4300i_COP1_W: break;
			case R4300i_COP1_L: break;
			default:
				_Notify->DisplayError("Unhandled R4300i OpCode in FillSectionInfo 2\n%s",
					R4300iOpcodeName(m_Command.Hex,m_PC));
				m_NextInstruction = END_BLOCK;
				m_PC -= 4;
			}
			break;
		case R4300i_BEQL: 
		case R4300i_BNEL: 
		case R4300i_BLEZL: 
		case R4300i_BGTZL: 
			m_NextInstruction = LIKELY_DELAY_SLOT;
#ifdef CHECKED_BUILD
			if (Section->m_Cont.TargetPC != m_PC + 8 && 
				Section->m_ContinueSection != NULL && 
				Section->m_Cont.TargetPC != (DWORD)-1)
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			}
			if (Section->m_Jump.TargetPC != m_PC + 4)
			{
				//_Notify->BreakPoint(__FILE__,__LINE__);
			}
			/*if (Section->m_Jump.TargetPC != m_PC + ((short)m_Command.offset << 2) + 4)
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			}*/
			if (m_PC == m_PC + ((short)m_Command.offset << 2) + 4) 
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix				
				if (!DelaySlotEffectsCompare(m_PC,m_Command.rs,m_Command.rt)) 
				{
					if (!Section->m_Jump.PermLoop)
					{
						_Notify->BreakPoint(__FILE__,__LINE__);
					}
				}
#endif
			} 
#endif
			break;
		case R4300i_DADDI: 
		case R4300i_DADDIU: 
			if (m_Command.rt == 0) { break; }
			if (m_Command.rs == m_Command.rt) 
			{
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);	
			}
			if (m_Reg.IsConst(m_Command.rs)) { 
				if (m_Reg.Is64Bit(m_Command.rs)) { 
					int imm32 = (short)m_Command.immediate;
					__int64 imm64 = imm32;										
					m_Reg.MipsReg_S(m_Command.rt) = m_Reg.MipsRegLo_S(m_Command.rs) + imm64;
				} else {
					m_Reg.MipsReg_S(m_Command.rt) = m_Reg.MipsRegLo_S(m_Command.rs) + (short)m_Command.immediate;
				}
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_CONST_64);
			} else {
				m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			}
			break;
		case R4300i_LDR:
		case R4300i_LDL:
		case R4300i_LB:
		case R4300i_LH: 
		case R4300i_LWL: 
		case R4300i_LW: 
		case R4300i_LWU: 
		case R4300i_LL: 
		case R4300i_LBU:
		case R4300i_LHU: 
		case R4300i_LWR: 
		case R4300i_SC: 
			if (m_Command.rt == 0) { break; }
			m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			break;
		case R4300i_SB: break;
		case R4300i_SH: break;
		case R4300i_SWL: break;
		case R4300i_SW: break;
		case R4300i_SWR: break;
		case R4300i_SDL: break;
		case R4300i_SDR: break;
		case R4300i_CACHE: break;
		case R4300i_LWC1: break;
		case R4300i_SWC1: break;
		case R4300i_LDC1: break;
		case R4300i_LD:
			if (m_Command.rt == 0) { break; }
			m_Reg.SetMipsRegState(m_Command.rt,CRegInfo::STATE_UNKNOWN);
			break;
		case R4300i_SDC1: break;
		case R4300i_SD: break;
		default:
			m_NextInstruction = END_BLOCK;
			m_PC -= 4;
			if (m_Command.Hex == 0x7C1C97C0) { break; }
			if (m_Command.Hex == 0x7FFFFFFF) { break; }
			if (m_Command.Hex == 0xF1F3F5F7) { break; }
			if (m_Command.Hex == 0xC1200000) { break; }
			if (m_Command.Hex == 0x4C5A5353) { break; }
			_Notify->DisplayError("Unhandled R4300i OpCode in FillSectionInfo 1\n%s\n%X",
				R4300iOpcodeName(m_Command.Hex,m_PC),m_Command.Hex);
		}

		if (Section->m_DelaySlot)
		{
			if (m_NextInstruction != NORMAL) { _Notify->BreakPoint(__FILE__,__LINE__); }
			m_NextInstruction = END_BLOCK;
			SetJumpRegSet(Section,m_Reg);
		} else {
			switch (m_NextInstruction) {
			case NORMAL: 
				m_PC += 4; 
				break;
			case DELAY_SLOT:
				m_NextInstruction = DELAY_SLOT_DONE;
				m_PC += 4; 
				break;
			case LIKELY_DELAY_SLOT:
				{
					SetContinueRegSet(Section,m_Reg);
					SetJumpRegSet(Section,m_Reg);
				}
				m_NextInstruction = END_BLOCK;
				break;
			case DELAY_SLOT_DONE:
				{
					SetContinueRegSet(Section,m_Reg);
					SetJumpRegSet(Section,m_Reg);
				}
				m_NextInstruction = END_BLOCK;
				break;
			case LIKELY_DELAY_SLOT_DONE:
				_Notify->BreakPoint(__FILE__,__LINE__);
				if (Section->m_CompiledLocation)
				{
				} else {
					//Section->m_Jump.RegSet = m_Reg;
					//Section->m_Jump.DoneDelaySlot = true; 
				}
				m_NextInstruction = END_BLOCK;
				break;
			}
		}

		if (m_PC == ContinueSectionPC) 
		{
			m_NextInstruction = END_BLOCK;
			SetContinueRegSet(Section,m_Reg);
		}

		if ((m_PC & 0xFFFFF000) != (m_EnterSection->m_EnterPC & 0xFFFFF000)) {
			if (m_NextInstruction != END_BLOCK && m_NextInstruction != NORMAL) 
			{
				_Notify->BreakPoint(__FILE__,__LINE__);
			} 
			_Notify->BreakPoint(__FILE__,__LINE__);
		}
	} while (m_NextInstruction != END_BLOCK);

	if (!CheckLoopRegisterUsage(Section->m_ContinueSection)) { return false; }
	if (!CheckLoopRegisterUsage(Section->m_JumpSection)) { return false; }

	if (!SetupEnterSection(Section)) { return false; }
	return true;
}

bool LoopAnalysis::SyncRegState ( CRegInfo & RegSet, const CRegInfo SyncReg )
{
	bool bChanged = false;
	for (int x = 0; x < 32; x++)
	{
		if (RegSet.MipsRegState(x) != SyncReg.MipsRegState(x))
		{
			CPU_Message(__FUNCTION__ ": Clear state %s RegEnter State: %X Jump Reg State: %X",CRegName::GPR[x],RegSet.MipsRegState(x),SyncReg.MipsRegState(x));
			RegSet.SetMipsRegState(x,CRegInfo::STATE_UNKNOWN);	
		}
		else if (RegSet.IsConst(x) && RegSet.Is32Bit(x) && RegSet.cMipsRegLo(x) != SyncReg.cMipsRegLo(x))
		{
			CPU_Message(__FUNCTION__ ": Clear state %s RegEnter State: %X Jump Reg State: %X",CRegName::GPR[x],RegSet.MipsRegState(x),SyncReg.MipsRegState(x));
			RegSet.SetMipsRegState(x,CRegInfo::STATE_UNKNOWN);	
		} else if (RegSet.IsConst(x) && RegSet.Is64Bit(x)) {
			_Notify->BreakPoint(__FILE__,__LINE__);
		}
	}
	return bChanged;
}

void LoopAnalysis::SetJumpRegSet ( CCodeSection * Section, const CRegInfo &Reg )
{
	RegisterMap::iterator itr = m_JumpRegisters.find(Section->m_SectionID);
	if (itr != m_JumpRegisters.end())
	{
		*(itr->second) = Reg;
	} else {
		m_JumpRegisters.insert(RegisterMap::value_type(Section->m_SectionID,new CRegInfo(Reg)));
	}
}

void LoopAnalysis::SetContinueRegSet ( CCodeSection * Section, const CRegInfo &Reg )
{
	RegisterMap::iterator itr = m_ContinueRegisters.find(Section->m_SectionID);
	if (itr != m_ContinueRegisters.end())
	{
		*(itr->second) = Reg;
	} else {
		m_ContinueRegisters.insert(RegisterMap::value_type(Section->m_SectionID,new CRegInfo(Reg)));
	}
}

void LoopAnalysis::SPECIAL_SLL ( void )
{
	if (m_Command.rd == 0) { return; }
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
}

void LoopAnalysis::SPECIAL_SRL ( void )
{
	if (m_Command.rd == 0) { return; }
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
}

void LoopAnalysis::SPECIAL_SRA ( void )
{
	if (m_Command.rd == 0) { return; }
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
}

void LoopAnalysis::SPECIAL_SLLV ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rt) << (m_Reg.MipsRegLo(m_Command.rs) & 0x1F);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_SRLV ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rt) >> (m_Reg.MipsRegLo(m_Command.rs) & 0x1F);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_SRAV ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo_S(m_Command.rt) >> (m_Reg.MipsRegLo(m_Command.rs) & 0x1F);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_JR ( void )
{
	_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
	if (m_Reg.IsConst(m_Command.rs)) {
		Section->m_Jump.TargetPC = m_Reg.MipsRegLo(m_Command.rs);
	} else {
		Section->m_Jump.TargetPC = (DWORD)-1;
	}
#endif
	m_NextInstruction = DELAY_SLOT;
}

void LoopAnalysis::SPECIAL_JALR ( void )
{
	_Notify->BreakPoint(__FILE__,__LINE__);
#ifdef tofix
	m_Reg.MipsRegLo(m_Command.rd) = m_PC + 8;
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
	if (m_Reg.IsConst(m_Command.rs)) {
		Section->m_Jump.TargetPC = m_Reg.MipsRegLo(m_Command.rs);
	} else {
		Section->m_Jump.TargetPC = (DWORD)-1;
	}
#endif
	m_NextInstruction = DELAY_SLOT;
}

void LoopAnalysis::SPECIAL_SYSCALL ( void )
{
	m_NextInstruction = END_BLOCK;
	m_PC -= 4;
}

void LoopAnalysis::SPECIAL_BREAK ( void )
{
	m_NextInstruction = END_BLOCK;
	m_PC -= 4;
}

void LoopAnalysis::SPECIAL_MFHI ( void )
{
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
}

void LoopAnalysis::SPECIAL_MTHI ( void )
{

}

void LoopAnalysis::SPECIAL_MFLO ( void )
{
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
}

void LoopAnalysis::SPECIAL_MTLO ( void )
{

}

void LoopAnalysis::SPECIAL_DSLLV ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.SetMipsReg(m_Command.rd, m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(QWORD)m_Reg.MipsRegLo_S(m_Command.rt) << (m_Reg.MipsRegLo(m_Command.rs) & 0x3F));
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSRLV ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.SetMipsReg(m_Command.rd,m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(QWORD)m_Reg.MipsRegLo_S(m_Command.rt) >> (m_Reg.MipsRegLo(m_Command.rs) & 0x3F));
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}

}

void LoopAnalysis::SPECIAL_DSRAV ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.SetMipsReg(m_Command.rd,m_Reg.Is64Bit(m_Command.rt)?m_Reg.cMipsReg_S(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt) >> (m_Reg.MipsRegLo(m_Command.rs) & 0x3F));
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_ADD ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rs) + m_Reg.MipsRegLo(m_Command.rt);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_ADDU ( void )
{
	if (m_Command.rd == 0) { return; }
	m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
}

void LoopAnalysis::SPECIAL_SUB ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rs) - m_Reg.MipsRegLo(m_Command.rt);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_SUBU ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rs) - m_Reg.MipsRegLo(m_Command.rt);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_AND ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		if (m_Reg.Is64Bit(m_Command.rt) && m_Reg.Is64Bit(m_Command.rs)) {
			m_Reg.SetMipsReg(m_Command.rd,m_Reg.MipsReg(m_Command.rt) & m_Reg.MipsReg(m_Command.rs));
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);					
		} else if (m_Reg.Is64Bit(m_Command.rt) || m_Reg.Is64Bit(m_Command.rs)) {
			if (m_Reg.Is64Bit(m_Command.rt)) {
				m_Reg.SetMipsReg(m_Command.rd, m_Reg.MipsReg(m_Command.rt) & m_Reg.MipsRegLo(m_Command.rs));
			} else {
				m_Reg.SetMipsReg(m_Command.rd, m_Reg.MipsRegLo(m_Command.rt) & m_Reg.MipsReg(m_Command.rs));
			}						
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::ConstantsType(m_Reg.MipsReg(m_Command.rd)));
		} else {
			m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rt) & m_Reg.MipsRegLo(m_Command.rs);
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		}
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_OR ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		if (m_Reg.Is64Bit(m_Command.rt) && m_Reg.Is64Bit(m_Command.rs)) {
			m_Reg.SetMipsReg(m_Command.rd,m_Reg.MipsReg(m_Command.rt) | m_Reg.MipsReg(m_Command.rs));
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		} else if (m_Reg.Is64Bit(m_Command.rt) || m_Reg.Is64Bit(m_Command.rs)) {
			if (m_Reg.Is64Bit(m_Command.rt)) {
				m_Reg.SetMipsReg(m_Command.rd,m_Reg.MipsReg(m_Command.rt) | m_Reg.MipsRegLo(m_Command.rs));
			} else {
				m_Reg.SetMipsReg(m_Command.rd,m_Reg.MipsRegLo(m_Command.rt) | m_Reg.MipsReg(m_Command.rs));
			}
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		} else {
			m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rt) | m_Reg.MipsRegLo(m_Command.rs);
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		}
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_XOR ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		if (m_Reg.Is64Bit(m_Command.rt) && m_Reg.Is64Bit(m_Command.rs)) {
			m_Reg.SetMipsReg(m_Command.rd,m_Reg.MipsReg(m_Command.rt) ^ m_Reg.MipsReg(m_Command.rs));
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		} else if (m_Reg.Is64Bit(m_Command.rt) || m_Reg.Is64Bit(m_Command.rs)) {
			if (m_Reg.Is64Bit(m_Command.rt)) {
				m_Reg.SetMipsReg(m_Command.rd, m_Reg.MipsReg(m_Command.rt) ^ m_Reg.MipsRegLo(m_Command.rs));
			} else {
				m_Reg.SetMipsReg(m_Command.rd, m_Reg.MipsRegLo(m_Command.rt) ^ m_Reg.MipsReg(m_Command.rs));
			}
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		} else {
			m_Reg.MipsRegLo(m_Command.rd) = m_Reg.MipsRegLo(m_Command.rt) ^ m_Reg.MipsRegLo(m_Command.rs);
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		}
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_NOR ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		if (m_Reg.Is64Bit(m_Command.rt) && m_Reg.Is64Bit(m_Command.rs)) {
			m_Reg.SetMipsReg(m_Command.rd,~(m_Reg.MipsReg(m_Command.rt) | m_Reg.MipsReg(m_Command.rs)));
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		} else if (m_Reg.Is64Bit(m_Command.rt) || m_Reg.Is64Bit(m_Command.rs)) {
			if (m_Reg.Is64Bit(m_Command.rt)) {
				m_Reg.SetMipsReg(m_Command.rd, ~(m_Reg.MipsReg(m_Command.rt) | m_Reg.MipsRegLo(m_Command.rs)));
			} else {
				m_Reg.SetMipsReg(m_Command.rd, ~(m_Reg.MipsRegLo(m_Command.rt) | m_Reg.MipsReg(m_Command.rs)));
			}
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		} else {
			m_Reg.MipsRegLo(m_Command.rd) = ~(m_Reg.MipsRegLo(m_Command.rt) | m_Reg.MipsRegLo(m_Command.rs));
			m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		}
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_SLT ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		if (m_Reg.Is64Bit(m_Command.rt) || m_Reg.Is64Bit(m_Command.rs)) {
			if (m_Reg.Is64Bit(m_Command.rt)) {
				m_Reg.MipsRegLo(m_Command.rd) = (m_Reg.MipsRegLo_S(m_Command.rs) < m_Reg.cMipsReg_S(m_Command.rt))?1:0;
			} else {
				m_Reg.MipsRegLo(m_Command.rd) = (m_Reg.cMipsReg_S(m_Command.rs) < m_Reg.MipsRegLo_S(m_Command.rt))?1:0;
			}
		} else {
			m_Reg.MipsRegLo(m_Command.rd) = (m_Reg.MipsRegLo_S(m_Command.rs) < m_Reg.MipsRegLo_S(m_Command.rt))?1:0;
		}
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_SLTU ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		if (m_Reg.Is64Bit(m_Command.rt) || m_Reg.Is64Bit(m_Command.rs)) {
			if (m_Reg.Is64Bit(m_Command.rt)) {
				m_Reg.MipsRegLo(m_Command.rd) = (m_Reg.MipsRegLo(m_Command.rs) < m_Reg.MipsReg(m_Command.rt))?1:0;
			} else {
				m_Reg.MipsRegLo(m_Command.rd) = (m_Reg.MipsReg(m_Command.rs) < m_Reg.MipsRegLo(m_Command.rt))?1:0;
			}
		} else {
			m_Reg.MipsRegLo(m_Command.rd) = (m_Reg.MipsRegLo(m_Command.rs) < m_Reg.MipsRegLo(m_Command.rt))?1:0;
		}
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DADD ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsReg(m_Command.rd, 
			m_Reg.Is64Bit(m_Command.rs)?m_Reg.MipsReg(m_Command.rs):(_int64)m_Reg.MipsRegLo_S(m_Command.rs) +
			m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt)
			);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DADDU ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsReg(m_Command.rd, 
			m_Reg.Is64Bit(m_Command.rs)?m_Reg.MipsReg(m_Command.rs):(_int64)m_Reg.MipsRegLo_S(m_Command.rs) +
			m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt)
			);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSUB ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsReg(m_Command.rd,
			m_Reg.Is64Bit(m_Command.rs)?m_Reg.MipsReg(m_Command.rs):(_int64)m_Reg.MipsRegLo_S(m_Command.rs) -
			m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt)
			);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSUBU ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd || m_Command.rs == m_Command.rd)
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt) && m_Reg.IsConst(m_Command.rs)) {
		m_Reg.SetMipsReg(m_Command.rd,
			m_Reg.Is64Bit(m_Command.rs)?m_Reg.MipsReg(m_Command.rs):(_int64)m_Reg.MipsRegLo_S(m_Command.rs) -
			m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt)
			);
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSLL ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd) 
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.SetMipsReg(m_Command.rd,m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt) << m_Command.sa);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSRL ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd) 
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.SetMipsReg(m_Command.rd, m_Reg.Is64Bit(m_Command.rt)?m_Reg.MipsReg(m_Command.rt):(QWORD)m_Reg.MipsRegLo_S(m_Command.rt) >> m_Command.sa);
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSRA ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd) 
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.MipsReg_S(m_Command.rd) = m_Reg.Is64Bit(m_Command.rt)?m_Reg.cMipsReg_S(m_Command.rt):(_int64)m_Reg.MipsRegLo_S(m_Command.rt) >> m_Command.sa;
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSLL32 ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd) 
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_64);
		m_Reg.SetMipsReg(m_Command.rd, m_Reg.MipsRegLo(m_Command.rt) << (m_Command.sa + 32));
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSRL32 ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd) 
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		m_Reg.MipsRegLo(m_Command.rd) = (DWORD)(m_Reg.MipsReg(m_Command.rt) >> (m_Command.sa + 32));
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}

void LoopAnalysis::SPECIAL_DSRA32 ( void )
{
	if (m_Command.rd == 0) { return; }
	if (m_Command.rt == m_Command.rd) 
	{
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);	
	}
	if (m_Reg.IsConst(m_Command.rt)) {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_CONST_32);
		m_Reg.MipsRegLo(m_Command.rd) = (DWORD)(m_Reg.cMipsReg_S(m_Command.rt) >> (m_Command.sa + 32));
	} else {
		m_Reg.SetMipsRegState(m_Command.rd,CRegInfo::STATE_UNKNOWN);
	}
}
