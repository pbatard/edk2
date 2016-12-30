## @file
# Insert EBC Call Signature data in an EFI executable
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
__version_number__ = ("1.0" + " " + gBUILD_VERSION)
__version__ = "%prog Version " + __version_number__
__copyright__ = "Copyright (c) 2016, Intel Corporation. All rights reserved."

#======================================  Internal Libraries ========================================

#============================================== Code ===============================================
EBC_CALL_SIGNATURE = 0x2EBC0000

CallRegexp = re.compile('^[\da-fA-F]+:[\da-fA-F]+ +([\w@\$]+)_plabel +([\da-fA-F]+) f? +([\w]+):([\w]+)\.obj', re.UNICODE)
LoadRegexp = re.compile('Preferred load address is +([\da-fA-F]+)', re.UNICODE)

def ParseMap(MapPath):
    """ Parse a map file and get the list and address of functions that need an EBC call signature
    @param MapPath    Map file absolute path

    @return a list of call names with their address and source location
    """
    Status = 0
    CallList = []

    with file(MapPath, 'r') as f:
        for Line in f:
            Line = Line.strip()

            if Status == 0:
                m = LoadRegexp.match(Line)
                if m != None:
                    LoadAddr, = m.groups(0)
                    LoadAddr = int(LoadAddr, 16)
                    Status = 1

            if Status == 1:
                m = CallRegexp.match(Line)
                if m != None:
                    FuncName, FuncAddr, ModName, SrcName = m.groups(0)
                    FuncAddr = int(FuncAddr, 16)
                    SrcName = SrcName + ".c"
                    CallList.append([FuncName, FuncAddr - LoadAddr, ModName, SrcName])

    assert Status != 0, "Failed to read load address"

    if len(CallList) == 0:
        return None
    return CallList

def BuildSignatureList(FileList):
    """ Parse the list of files passed as arguments, and try to locate and open
        the matching signature files to build a complete EBC signature list.
        as the signature file to ensure that the signatures we need are present.
    @param FileList     A list of .sig or .lib files

    @return a list of function calls with their signature data
    """
    global Options
    SigList = {}

    for File in FileList:
        File = os.path.splitext(File)[0]+'.sig'
        try:
            with open(File, 'rb') as f:
                SavedList = pickle.load(f)
                SigList.update(SavedList)
                if Options.verbose != None:
                    print "  Loaded " + str(len(SavedList)) + " EBC signature(s) from " + File
        except:
            pass

    if len(SigList) == 0:
        return None
    return SigList

def CheckSignatures(EfiPath, MapList, SigList):
    """ Sanity check to ensures that the signatures' data and addresses are valid.
    @param EfiPath      EFI binary absolute path
    @param MapList      Function calls requiring signature, with their address
    @param SigList      Function calls signature dictionary

    @return True if the check passed
    """

    for Entry in MapList:
        # Check for missing signatures
        assert Entry[0] in SigList, Entry[0] + ": missing signature"
        # Make sure the signature fits in 16 bits
        assert SigList[Entry[0]] < 0x10000, Entry[0] + ": invalid signature"

    with file(EfiPath, 'rb') as f:
        for Entry in MapList:
            f.seek(Entry[1] + 4)
            Data = struct.unpack('I', f.read(4))[0]
            # The 32 bit data should either be 0 or have the call signature marker
            assert Data == 0 or Data & 0xFFFF0000 == EBC_CALL_SIGNATURE, "Unexpected data at address 0x%x" % Entry[1]

    return True;

def InsertSignatures(EfiPath, MapList, SigList):
    """ Check the EFI binary to ensure we have the right signature addresses
    @param EfiPath      EFI binary absolute path
    @param Maplist      Function calls requiring signature, with their address
    @param SigList      Function calls signature dictionary

    @return True if the signatures were successfully patched
    """
    global Options

    with file(EfiPath, 'r+b') as f:
        Width = max(len(Entry[0]) for Entry in MapList) + 3
        for Entry in MapList:
            f.seek(Entry[1] + 4)
            f.write(struct.pack('I', EBC_CALL_SIGNATURE + SigList[Entry[0]]))
            if Options.verbose != None:
                print "  Patched " + (Entry[0] + "()").ljust(Width) + "at 0x{:08X}".format(Entry[1]) + " with signature 0x{:04X}".format(SigList[Entry[0]]) + " (" + Entry[2] + ":" + Entry[3] + ")"

    return True;

def ParseOptions():
    Usage = "%prog [-v] -m <MapFile> -e <EfiFile> -s <SigFile> [file1] [file2] [...]"
    AdditionalNotes = "\nfile# can be either a .sig or .lib file. If the latter, its path is used to look for a matching .sig."
    Parser = optparse.OptionParser(usage=Usage, description=__copyright__, version="%prog " + __version_number__)
    Parser.add_option('-m', '--mapfile', action='store', dest='mapfile', type="string", help='Path of the module map file.')
    Parser.add_option('-e', '--efifile', action='store', dest='efifile', type="string", help='Path of the EFI binary to patch.')
    Parser.add_option("-v", "--verbose", action="store_true", type=None, help="Turn on verbose output with informational messages printed.")
    Parser.add_option("-q", "--quiet", action="store_true", type=None, help="Disable all messages except FATAL ERRORS.")
    Parser.add_option("-d", "--debug", action="store", type="int", help="Enable debug messages at specified level.")

    return Parser.parse_args()

def main():
    global Options
    AppName = os.path.basename(sys.argv[0])
    (Options, ArgList) = ParseOptions()
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

        if Options.mapfile == None:
            EdkLogger.error(AppName, OPTION_MISSING, "Map file not defined",
                ExtraData="Please use '-m' switch to define a map file.")
        if Options.efifile == None:
            EdkLogger.error(AppName, OPTION_MISSING, "EFI file not defined",
                ExtraData="Please use '-e' switch to set the EFI binary to patch.")
        if ArgList == None:
            EdkLogger.error(AppName, OPTION_MISSING, "No signature or library file provided",
                ExtraData="At least one signature or library path must be provided as argument.")

        MapList = ParseMap(Options.mapfile)
        if MapList != None:
            SigList = BuildSignatureList(ArgList)
            assert SigList != None, "No signatures found"
            CheckSignatures(Options.efifile, MapList, SigList)
            if Options.debug != None:
                print "EBC signatures:"
                Width = max(len(CallName) for CallName in SigList) + 3
                for CallName in SigList:
                    print "  " + (CallName + "()").ljust(Width) + "0b" + "{:016b}".format(SigList[CallName])
            InsertSignatures(Options.efifile, MapList, SigList)
        elif Options.verbose:
            print 'No EBC call signatures patching required'

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
