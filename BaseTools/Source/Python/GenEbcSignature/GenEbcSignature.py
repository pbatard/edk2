## @file
# Generate a list of EBC call signatures
#
# Copyright (c) 2008 - 2016, Intel Corporation. All rights reserved.<BR>
# Copyright (c) 2016, Pete Batard. All rights reserved.<BR>
# This program and the accompanying materials
# are licensed and made available under the terms and conditions of the BSD License
# which accompanies this distribution.  The full text of the license may be found at
# http://opensource.org/licenses/bsd-license.php
#
# THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
# WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
#
#

#======================================  External Libraries ========================================
import optparse
import Common.LongFilePathOs as os
import re
import sys
import array
import struct
import pickle

from Common.BuildToolError import *
import Common.EdkLogger as EdkLogger
from Common.Misc import PeImageClass
from Common.BuildVersion import gBUILD_VERSION
from Common.LongFilePathSupport import OpenLongFilePath as open

# Version and Copyright
__version_number__ = ("0.5" + " " + gBUILD_VERSION)
__version__ = "%prog Version " + __version_number__
__copyright__ = "Copyright (c) 2016, Intel Corporation. All rights reserved."

#======================================  Internal Libraries ========================================

#============================================== Code ===============================================
EBC_CALL_SIGNATURE = 0x2EBC0000

functionStartRe = re.compile('^\s*(\w+)\s+\(\s*', re.UNICODE)
param64BitRe = re.compile('\\bU*INT64\\b(?!\s*\*)\s*\w+\\b', re.UNICODE)

def ParseSource(CallList):
    """ Parse preprocessed source to generate a function signature.

    Note that this is a very basic parser, that expects all function definitions to follow the
    UEFI coding standard, i.e. with function name and each parameters on individual lines.
    It also expects 64 bit parameter to be explicitly passed as INT64 or UINT64.

    @return the processed function call list with their signatures set
    """
    global Options
    Status = 0
    Signature = 0
    FuncName = ""

    for Line in sys.stdin:
        Line = Line.strip()

        if Status == 0:
            m = functionStartRe.match(Line)
            if m != None:
                FuncName, = m.groups(0)
                if FuncName in CallList:
                    Status = 1
                    Mask = 1
                    Signature = 0
                    if Options.debug > 1:
                        print FuncName + "("
                continue

        if Status == 1 and Line[0] == ')':
            if Options.debug > 1:
                print ")"
            CallList[FuncName] = Signature
            Status = 0
        if Status == 1:
            if Options.debug > 1:
                print "     " + Line
            m = param64BitRe.match(Line)
            if m != None:
                Signature = Signature | Mask
            Mask = Mask << 1

    return True

def ParseObject(ObjPath):
    """ Parse a COFF object file to identify function calls that require signature generation
    @param ObjPath    COFF object absolute path

    @return a list of function call names require signature generation
    """
    global Options
    CallList = {}
    RelocList = []
    SymbolList = {}

    with file(ObjPath, 'rb') as f:
        # Parse the object header
        Machine = struct.unpack('H', f.read(2))[0]
        assert Machine == 0xEBC, "Not an EBC object file"
        NumSections = struct.unpack('H', f.read(2))[0]
        f.seek(4, 1)    # Skip timestamp
        SymTabAddr = struct.unpack('I', f.read(4))[0]
        NumSymbols = struct.unpack('I', f.read(4))[0]
        OptHeaderSize = struct.unpack('H', f.read(2))[0]
        assert OptHeaderSize == 0, "Unexpected object file header"
        Characteristics = struct.unpack('H', f.read(2))[0]
        StrTabAddr = SymTabAddr + NumSymbols * 18
        # Optional debug output
        if Options.debug > 0:
            print "Number of Sections: " + str(NumSections)
            print "Number of Symbols:  " + str(NumSymbols)
            print "Symbol Table at: " + hex(SymTabAddr)
            print "String Table at: " + hex(StrTabAddr)

        # Parse the object sections, to identify reloc sections
        for i in range (0, NumSections):
            SectionName = struct.unpack('8s', f.read(8))[0]
            SectionName = SectionName.strip()
            VirtualSize = struct.unpack('I', f.read(4))[0]
            assert VirtualSize == 0, "Unexpected Obj file section"
            # Skip Virtual Address
            f.seek(4, 1)
            DataSize = struct.unpack('I', f.read(4))[0]
            DataAddr = struct.unpack('I', f.read(4))[0]
            RelocAddr = struct.unpack('I', f.read(4))[0]
            # Skip Line Numbers
            f.seek(4, 1)
            RelocNum = struct.unpack('H', f.read(2))[0]
            f.seek(6, 1)
            if RelocNum > 0:
                RelocList.append([RelocNum, RelocAddr])
            if Options.debug > 1:
                print "  Section " + str(i) + ": " + SectionName
                print "    Size: " + hex(DataSize)
                print "    Addr: " + hex(DataAddr)
                if RelocNum > 0:
                    print "    Rels: " + str(RelocNum) + " at " + hex(RelocAddr)

        # Parse reloc sections
        if RelocList != None:
            # Copy the String Table into something we can access more easily
            f.seek(StrTabAddr, 0)
            StrTabSize = struct.unpack('I', f.read(4))[0]
            StrData = f.read(StrTabSize)
            if Options.debug > 0:
                print "String Table Size: " + str(StrTabSize)
                if Options.debug > 1:
                    StrArray = StrData.split(b'\x00')
                    for String in StrArray:
                        print "    " + String

            # Now build a full symbol table, that includes long names
            f.seek(SymTabAddr, 0)
            NumAuxSections = 0
            if Options.debug > 0 and NumSymbols > 0:
                print "Relocation symbols:"
            for i in range(0, NumSymbols):
                if NumAuxSections > 0:
                    f.seek(18, 1)
                    NumAuxSections = NumAuxSections - 1
                    continue
                SymName = f.read(8)
                SymValue = struct.unpack('I', f.read(4))[0]
                SymSecNum = struct.unpack('h', f.read(2))[0]
                SymType = struct.unpack('H', f.read(2))[0]
                SymClass = struct.unpack('B', f.read(1))[0]
                NumAuxSections = struct.unpack('B', f.read(1))[0]
                if SymName[0] != '\0':
                   SymName = SymName.strip(' \0')
                else:
                   StrIndex = struct.unpack('I', SymName[4:8])[0]
                   StrEnd = StrData.find('\0', StrIndex-3)
                   SymName = StrData[StrIndex-4:StrEnd]
                SymbolList[i] = SymName
                if Options.debug > 1:
                    print "    " + SymName + ":"
                    print "      Index=" + str(i) + " SecNum=" + str(SymSecNum) + " Type=" + str(SymType) + " Class=" + str(SymClass) + " NbAux=" + str(NumAuxSections)

            # Finally, go through each reloc symbol and identify the "_plabel" suffixed ones
            for Entry in RelocList:
                f.seek(Entry[1], 0)

                for i in range(0, Entry[0]):
                    # Skip Virtual Address
                    f.seek(4, 1)
                    SymbolIndex = struct.unpack('I', f.read(4))[0]
                    assert SymbolIndex in SymbolList, "Symbol not found in list"
                    RelocType = struct.unpack('H', f.read(2))[0]
                    m = re.match('(.*)_plabel', SymbolList[SymbolIndex])
                    if m != None:
                        CallName, = m.groups(0)
                        if CallName not in CallList:
                            # Add this call, and set its signature to undefined (-1)
                            CallList[CallName] = -1
                    if Options.debug > 0:
                         print "    " + SymbolList[SymbolIndex] + " (Index " + hex(SymbolIndex) + ", Type " + str(RelocType) + ")"

    if len(CallList) == 0:
        return None
    return CallList

def UpdateSignatures(CallList, SigFile):
    """ Create or update a signature file
    @param CallList   A dictionary containing the signatures to save/update
    @param SigFile    A path to the signature file to create/update

    @return True if successfull
    """
    SavedList = {}

    if Options.verbose != None:
        print "EBC signatures:"
        Width = max(len(CallName) for CallName in CallList) + 3
        for CallName in CallList:
            print "  " + (CallName + "()").ljust(Width) + "0b" + "{:016b}".format(CallList[CallName])

    try:
        f = open(SigFile, 'rb')
        SavedList = pickle.load(f)
        f.close()

    except:
        pass

    SavedList.update(CallList)
    with open(Options.sigfile, 'wb') as f:
        pickle.dump(SavedList, f, protocol=pickle.HIGHEST_PROTOCOL)

    return True

def ParseOptions():
    Usage = "%prog [-v] -o <ObjFile> -s <SigFile>"
    AdditionalNotes = "\nThis application currently only recognizes INT64 or UINT64 types as 64-bit arg."
    Parser = optparse.OptionParser(usage=Usage, description=__copyright__, version="%prog " + __version_number__)
    Parser.add_option('-o', '--objfile', action='store', dest='objfile', type="string", help='Path of the object file.')
    Parser.add_option('-s', '--sigfile',  action='store', dest='sigfile', type="string", help='Path to the signature file.')
    Parser.add_option("-v", "--verbose", action="store_true", type=None, help="Turn on verbose output with informational messages printed.")
    Parser.add_option("-q", "--quiet", action="store_true", type=None, help="Disable all messages except FATAL ERRORS.")
    Parser.add_option("-d", "--debug", action="store", type="int", help="Enable debug messages at specified level.")

    (options, args) = Parser.parse_args()
    return options

def main():
    global Options
    AppName = os.path.basename(sys.argv[0])
    Options = ParseOptions()
    ReturnCode = 0

    EdkLogger.Initialize()
    try:
        if Options.verbose != None:
            EdkLogger.SetLevel(EdkLogger.VERBOSE)
        if Options.quiet != None:
            EdkLogger.SetLevel(EdkLogger.QUIET)
        if Options.debug != None:
            EdkLogger.SetLevel(Options.debug + 1)
        else:
            EdkLogger.SetLevel(EdkLogger.INFO)

        if Options.objfile == None:
            EdkLogger.error(AppName, OPTION_MISSING, "Object file not defined",
                ExtraData="Please use '-o' switch to set the input Object file.")
        if Options.sigfile == None:
            EdkLogger.error(AppName, OPTION_MISSING, "Signature file not defined",
                ExtraData="Please use '-s' switch to set the output Signature file.")

        List = ParseObject(Options.objfile)
        if List != None:
            ParseSource(List)
            # Check for missing signatures
            for CallName in List:
                assert List[CallName] != -1, "Could not set signature for function " + CallName
            UpdateSignatures(List, Options.sigfile)
        else:
            # Still need to deplete stdin data
            for Line in sys.stdin:
                pass

    except FatalError, X:
        if Options.debug != None:
            import traceback
            EdkLogger.quiet(traceback.format_exc())
        ReturnCode = X.args[0]

    except:
        import traceback
        EdkLogger.error(
            "\nPython",
            CODE_ERROR,
            "Tools code failure",
            ExtraData="Please send email to edk2-devel@lists.01.org for help, attaching following call stack trace!\n",
            RaiseError=False
        )
        EdkLogger.quiet(traceback.format_exc())
        ReturnCode = CODE_ERROR
    return ReturnCode

if __name__ == '__main__':
    r = main()
    ## 0-127 is a safe return range, and 1 is a standard default error
    if r < 0 or r > 127: r = 1
    sys.exit(r)
