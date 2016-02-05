/*
 * Copyright (c) 2015, Xerox Corporation (Xerox)and Palo Alto Research Center (PARC)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Patent rights are not granted under this agreement. Patent rights are
 *       available under FRAND terms.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL XEROX or PARC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @author Alan Walendowski, Palo Alto Research Center (Xerox PARC)
 * @copyright 2015, Xerox Corporation (Xerox)and Palo Alto Research Center (PARC).  All rights reserved.
 */

#include "tutorial_Common.h"
#include "tutorial_About.h"

#include <LongBow/runtime.h>
#include <stdio.h>

#include <ccnx/common/ccnx_NameSegmentNumber.h>

#include <parc/security/parc_Security.h>
#include <parc/security/parc_PublicKeySignerPkcs12Store.h>
#include <parc/security/parc_IdentityFile.h>

/**
 * The default name the tutorial will use if no other name is specified.
 */
static const char _defaultTutorialDomainPrefix[] = "lci:/ccnx/tutorial";

/**
 * The CCNx Name prefix we'll use for the tutorial.
 */
char tutorialCommon_DomainPrefix[512]; 

/**
 * The size of a chunk. We break CCNx Content payloads up into pieces of this size.
 * 1200 was chosen as a size that should prevent IP fragmentation of CCNx ContentObject Messages.
 */
const uint32_t tutorialCommon_ChunkSize = 1200;

/**
 * The string we use for the 'fetch' command.
 */
const char *tutorialCommon_CommandFetch = "fetch";

/**
 * The string we use for the 'list' command.
 */
const char *tutorialCommon_CommandList = "list";

PARCIdentity *
tutorialCommon_CreateAndGetIdentity(const char *keystoreName, const char *keystorePassword, const char *subjectName)
{
    parcSecurity_Init();

    unsigned int keyLength = 1024;
    unsigned int validityDays = 30;

    bool success = parcPublicKeySignerPkcs12Store_CreateFile(keystoreName, keystorePassword, subjectName, keyLength, validityDays);
    assertTrue(success,
               "parcPublicKeySignerPkcs12Store_CreateFile('%s', '%s', '%s', %d, %d) failed.",
               keystoreName, keystorePassword, subjectName, keyLength, validityDays);

    PARCIdentityFile *identityFile = parcIdentityFile_Create(keystoreName, keystorePassword);
    PARCIdentity *result = parcIdentity_Create(identityFile, PARCIdentityFileAsPARCIdentity);
    parcIdentityFile_Release(&identityFile);

    parcSecurity_Fini();

    return result;
}

CCNxPortalFactory *
tutorialCommon_SetupPortalFactory(const char *keystoreName, const char *keystorePassword, const char *subjectName)
{
    PARCIdentity *identity = tutorialCommon_CreateAndGetIdentity(keystoreName, keystorePassword, subjectName);
    CCNxPortalFactory *result = ccnxPortalFactory_Create(identity);
    parcIdentity_Release(&identity);

    return result;
}

CCNxName *
tutorialCommon_CreateWithBaseName(const CCNxName *name)
{
    size_t numberOfSegmentsInName = ccnxName_GetSegmentCount(name);

    CCNxName *result = ccnxName_Create();
    
    // Copy all segments, except the last one - which is the chunk number.
    for (int i = 0; i < numberOfSegmentsInName-1; i++) {
        ccnxName_Append(result, ccnxName_GetSegment(name, i));
    }
    
    return result;
}

uint64_t
tutorialCommon_GetChunkNumberFromName(const CCNxName *name)
{
    size_t numberOfSegmentsInName = ccnxName_GetSegmentCount(name);
    CCNxNameSegment *chunkNumberSegment = ccnxName_GetSegment(name, numberOfSegmentsInName - 1);

    assertTrue(ccnxNameSegment_GetType(chunkNumberSegment) == CCNxNameLabelType_CHUNK,
               "Last segment is the wrong type, expected CCNxNameLabelType %02X got %02X",
               CCNxNameLabelType_CHUNK,
               ccnxNameSegment_GetType(chunkNumberSegment)) {
        ccnxName_Display(name, 0); // This executes only if the enclosing assertion fails
    }

    return ccnxNameSegmentNumber_Value(chunkNumberSegment);
}


char *
tutorialCommon_CreateFileNameFromName(const CCNxName *name)
{
    // For the Tutorial, the second to last NameSegment is the filename.
    CCNxNameSegment *fileNameSegment = ccnxName_GetSegment(name, ccnxName_GetSegmentCount(name) - 2); // '-2' because we want the second to last segment

    assertTrue(ccnxNameSegment_GetType(fileNameSegment) == CCNxNameLabelType_NAME,
               "Last segment is the wrong type, expected CCNxNameLabelType %02X got %02X",
               CCNxNameLabelType_NAME,
               ccnxNameSegment_GetType(fileNameSegment)) {
        ccnxName_Display(name, 0); // This executes only if the enclosing assertion fails
    }

    return ccnxNameSegment_ToString(fileNameSegment); // This memory must be freed by the caller.
}

char *
tutorialCommon_CreateCommandStringFromName(const CCNxName *name, const CCNxName *domainPrefix)
{
    // For the Tutorial, the NameSegment immediately following the domain prefix contains the command.
    CCNxNameSegment *commandSegment = ccnxName_GetSegment(name, ccnxName_GetSegmentCount(domainPrefix));

    assertTrue(ccnxNameSegment_GetType(commandSegment) == CCNxNameLabelType_NAME,
               "Last segment is the wrong type, expected CCNxNameLabelType %02X got %02X",
               CCNxNameLabelType_NAME,
               ccnxNameSegment_GetType(commandSegment)) {
        ccnxName_Display(name, 0); // This executes only if the enclosing assertion fails
    }

    return ccnxNameSegment_ToString(commandSegment); // This memory must be freed by the caller.
}

int
tutorialCommon_processCommandLineArguments(int argc, char **argv,
                                           int *commandArgCount, char **commandArgs,
                                           bool *needToShowUsage, bool *shouldExit)
{
    int status = EXIT_SUCCESS;
    *commandArgCount = 0;
    *needToShowUsage = false;
    int got_nd = 0; // got the domainPrefix definition ...

    for (size_t i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] == '-') {
            switch (arg[1]) {
                case 'l': {
                    if (argv[2]) {
                        strncpy(tutorialCommon_DomainPrefix, argv[2], sizeof(tutorialCommon_DomainPrefix));
                        printf("Using domainPrefix :: [%s]\n", tutorialCommon_DomainPrefix);
                        got_nd = 1;     
                        i++;
                    }
                    break;
                }
                case 'h': {
                    *needToShowUsage = true;
                    *shouldExit = true;
                    break;
                }
                case 'v': {
                    printf("%s version: %s\n", argv[0], tutorialAbout_Version());
                    *shouldExit = true;
                    break;
                }
                default: { // Unexpected '-' option.
                    *needToShowUsage = true;
                    *shouldExit = true;
                    status = EXIT_FAILURE;
                    break;
                }
            }
        } else {
            // Not a '-' option, so save it as a command argument.
            commandArgs[(*commandArgCount)++] = arg;
        }
    }

    if (got_nd == 0) {
        strncpy(tutorialCommon_DomainPrefix, _defaultTutorialDomainPrefix, sizeof(tutorialCommon_DomainPrefix));
        printf("Using the default domainPrefix :: [%s]\n", tutorialCommon_DomainPrefix);
    }
    return status;
}
