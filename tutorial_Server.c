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
 * @author Glenn Scott, Alan Walendowski, Palo Alto Research Center (Xerox PARC)
 * @copyright 2015, Xerox Corporation (Xerox)and Palo Alto Research Center (PARC).  All rights reserved.
 */

#include <LongBow/runtime.h>

#include <strings.h>
#include <stdio.h>

#include "tutorial_Common.h"
#include "tutorial_FileIO.h"
#include "tutorial_About.h"


#include <ccnx/api/ccnx_Portal/ccnx_Portal.h>
#include <ccnx/api/ccnx_Portal/ccnx_PortalRTA.h>

#include <parc/algol/parc_Memory.h>
#include <parc/algol/parc_ArrayList.h>

#include <ccnx/common/ccnx_Name.h>
#include <ccnx/common/ccnx_ContentObject.h>
#include <ccnx/common/ccnx_NameSegmentNumber.h>

typedef struct _pre_chunked_content {
    char *fileName;
    PARCArrayList *chunks;
} ChunkList;

static PARCArrayList *_chunksByFileName = NULL;

/**
 * Create a new CCNxPortalFactory instance using a randomly generated identity saved to
 * the specified keystore.
 *
 * @return A new CCNxPortalFactory instance which must eventually be released by calling ccnxPortalFactory_Release().
 */
static CCNxPortalFactory *
_setupServerPortalFactory(void)
{
    const char *keystoreName = "tutorialServer_keystore";
    const char *keystorePassword = "keystore_password";
    const char *subjectName = "tutorialServer";

    return tutorialCommon_SetupPortalFactory(keystoreName, keystorePassword, subjectName);
}

/**
 * Given the size of some data and a chunk size, calculate the number of chunks that would be
 * required to contain the data.
 *
 * @param [in] dataLength The size of the data being chunked.
 * @param [in] chunkSize The size of the chunks to break the data in to.
 *
 * @return The number of chunks required to contain the specified data length.
 */
static uint64_t
_getNumberOfChunksRequired(uint64_t dataLength, uint32_t chunkSize)
{
    uint64_t chunks = (dataLength / chunkSize) + (dataLength % chunkSize > 0 ? 1 : 0);
    return (chunks == 0) ? 1 : chunks;
}

/**
 * Given the full path to a file, calculate and return the number of the final chunk in the file.
 * The final chunk nunber is a function of the size of the file and the specified chunk size. It
 * is 0-based and is never negative. A file of size 0 has a final chunk number of 0.
 *
 * @param [in] filePath The full path to a file.
 * @param [in] chunkSize The size of the chunks to break the file in to.
 *
 * @return The number of the final chunk required to transfer the specified file.
 */
static uint64_t
_getFinalChunkNumberOfFile(const char *filePath, uint32_t chunkSize)
{
    size_t fileSize = tutorialFileIO_GetFileSize(filePath);
    uint64_t totalNumberOfChunksInFile = _getNumberOfChunksRequired(fileSize, chunkSize);

    // If the file size == 0, the the final chunk number is 0. Else, it's one less
    // than the number of chunks in the file.

    return totalNumberOfChunksInFile > 0 ? (totalNumberOfChunksInFile - 1) : 0;
}

/**
 * Given a Name, a payload, and the number of the last chunk, create a CCNxContentObject suitable for
 * passing to the Portal. This new CCNxContentObject must eventually be released by calling
 * ccnxContentObject_Release().
 *
 * @param name [in] The CCNxName to use when creating the new ContentObject.
 * @param payload [in] A PARCBuffer to use as the payload of the new ContentObject.
 * @param finalChunkNumber [in] The number of the final chunk that will be required to completely transfer
 *        the requested content.
 *
 * @return A newly created CCNxContentObject with the specified name, payload, and finalChunkNumber.
 */
static CCNxContentObject *
_createContentObject(const CCNxName *name, PARCBuffer *payload, uint64_t finalChunkNumber)
{
    // In the call below, we are un-const'ing name for ccnxContentObject_CreateWithDataPayload()
    // but we will not be changing it.
    CCNxContentObject *result = ccnxContentObject_CreateWithDataPayload((CCNxName *) name, payload);
    ccnxContentObject_SetFinalChunkNumber(result, finalChunkNumber);

    return result;
}

/**
 * Given a CCNxName, a directory path, a file name, and a requested chunk number, return a new CCNxContentObject
 * with that CCNxName and containing the specified chunk of the file. The new CCNxContentObject will also
 * contain the number of the last chunk required to transfer the complete file. Note that the last chunk of the
 * file being retrieved is calculated each time we retrieve a chunk so the file can be growing in size as we
 * transfer it.
 * The new CCnxContentObject must eventually be released by calling ccnxContentObject_Release().
 *
 * @param [in] name The CCNxName to use when creating the new CCNxContentObject.
 * @param [in] directoryPath The directory in which to find the specified file.
 * @param [in] fileName The name of the file.
 * @param [in] requestedChunkNumber The number of the requested chunk from the file.
 *
 * @return A new CCNxContentObject instance containing the request chunk of the specified file, or NULL if
 *         the file did not exist or was otherwise unavailable.
 */
static CCNxContentObject *
_createFetchResponse(const CCNxName *name, const char *directoryPath, const char *fileName, uint64_t requestedChunkNumber)
{
    CCNxContentObject *result = NULL;
    uint64_t finalChunkNumber = 0;

    // Combine the directoryPath and fileName into the full path name of the desired file
    size_t filePathBufferSize = strlen(fileName) + strlen(directoryPath) + 2; // +2 for '/' and trailing null.
    char *fullFilePath = parcMemory_Allocate(filePathBufferSize);
    assertNotNull(fullFilePath, "parcMemory_Allocate(%zu) returned NULL", filePathBufferSize);
    snprintf(fullFilePath, filePathBufferSize, "%s/%s", directoryPath, fileName);

    // Make sure the file exists and is accessible before creating a ContentObject response.
    if (tutorialFileIO_IsFileAvailable(fullFilePath)) {
        // Since the file's length can change (e.g. if it is being written to while we're fetching
        // it), the final chunk number can change between requests for content chunks. So, update
        // it each time this function is called.
        finalChunkNumber = _getFinalChunkNumberOfFile(fullFilePath, tutorialCommon_ChunkSize);

        // Get the actual contents of the specified chunk of the file.
        PARCBuffer *payload = tutorialFileIO_GetFileChunk(fullFilePath, tutorialCommon_ChunkSize, requestedChunkNumber);

        if (payload != NULL) {
            result = _createContentObject(name, payload, finalChunkNumber);
            parcBuffer_Release(&payload);
        }
    }

    parcMemory_Deallocate((void **) &fullFilePath);

    return result; // Could be NULL if there was no payload
}


PARCArrayList *
_chunkFileIntoMemory(char *fullFilePath, const CCNxName *name)
{
    PARCArrayList *result = NULL;

    printf("Pre-chunking %s into memory...\n", fullFilePath);

    // Make sure the file exists and is accessible before creating a ContentObject response.
    if (tutorialFileIO_IsFileAvailable(fullFilePath)) {
        uint64_t finalChunkNumber = _getFinalChunkNumberOfFile(fullFilePath, tutorialCommon_ChunkSize);

        CCNxName *baseName = tutorialCommon_CreateWithBaseName(name);
        result = parcArrayList_Create_Capacity(NULL, NULL, finalChunkNumber);

        // Get a copy of the name, but without the chunk number.

        //printf("Chunking [%s], %ld chunks\n", fullFilePath, finalChunkNumber);
        for (uint64_t i = 0; i <= finalChunkNumber; i++) {
            // Get the actual contents of the specified chunk of the file.
            PARCBuffer *payload = tutorialFileIO_GetFileChunk(fullFilePath, tutorialCommon_ChunkSize, i);

            if (payload != NULL) {
                CCNxName *chunkName = ccnxName_Copy(baseName);
                CCNxNameSegment *chunkSegment = ccnxNameSegmentNumber_Create(CCNxNameLabelType_CHUNK, i);
                ccnxName_Append(chunkName, chunkSegment);

                CCNxContentObject *contentObject = _createContentObject(chunkName, payload, finalChunkNumber);

                parcBuffer_Release(&payload);
                ccnxName_Release(&chunkName);
                ccnxNameSegment_Release(&chunkSegment);

                parcArrayList_Add(result, contentObject);
            } else {
                trapUnexpectedState("Could not get required chunk");
            }
        }
        ccnxName_Release(&baseName);
        printf("Finished chunking %s into memory. Resulted in %ld content objects.\n", fullFilePath, finalChunkNumber);
    } else {
        //trapUnexpectedState("Could not open file %s for chunking.", fullFilePath);

        printf("## !! ## Could not access requested file [%s]. Could not pre-chunk. ## !! ##\n", fullFilePath);
    }

    return result;
}

/**
 * Same as _createFetchResponse(), but pre-calculates ALL of the content objects and stores them in memory for quick retrieval.
 */
static CCNxContentObject *
_createFetchResponseWithPreChunking(const CCNxName *name, const char *directoryPath, char *fileName, uint64_t requestedChunkNumber)
{
    CCNxContentObject *result = NULL;

    ChunkList *movieChunks = NULL;

    //printf("Asking for chunk %ld of %s\n", requestedChunkNumber, fullFilePath);

    size_t numFilesChunked = parcArrayList_Size(_chunksByFileName);
    for (int i = 0; i < numFilesChunked; i++) {
        movieChunks = parcArrayList_Get(_chunksByFileName, i);
        //printf("[%d] (%p) testing [%s] agains [%s]\n", i, (void *)movieChunks, movieChunks->fileName, fileName);
        // use fileName and not fullFilePath for a strcmp optimization. This should really be the ccnx name, but...
        if (strcmp(movieChunks->fileName, fileName) == 0) {
            //printf("Found: [%d] %s (matched %s)\n", i, movieChunks->fileName, fileName);
            break;
        } else {
            movieChunks = NULL;
        }
    }

    if (movieChunks == NULL) {
        // Chunk list for this file was empty. Build it. This will take a while.

        // Combine the directoryPath and fileName into the full path name of the desired file
        size_t filePathBufferSize = strlen(fileName) + strlen(directoryPath) + 2; // +2 for '/' and trailing null.
        char *fullFilePath = parcMemory_Allocate(filePathBufferSize);
        assertNotNull(fullFilePath, "parcMemory_Allocate(%zu) returned NULL", filePathBufferSize);
        snprintf(fullFilePath, filePathBufferSize, "%s/%s", directoryPath, fileName);

        printf("Pre-Chunking: %s ... \n", fullFilePath);
        PARCArrayList *chunkedContent = _chunkFileIntoMemory(fullFilePath, name);

        if (chunkedContent != NULL) {
            printf("\nFinished Pre-Chunking: %s\n", fullFilePath);

            movieChunks = parcMemory_Allocate(sizeof(ChunkList));
            movieChunks->chunks = chunkedContent;
            movieChunks->fileName = parcMemory_StringDuplicate(fileName, strlen(fileName)+1);

            printf("Creating ChunkList %p for [%s], chunk array is %p\n", movieChunks, movieChunks->fileName, movieChunks->chunks);
            parcArrayList_Add(_chunksByFileName, movieChunks);

            // Debugging:
            //size_t numFilesChunked = parcArrayList_Size(_chunksByFileName);
            //for (int i = 0; i < numFilesChunked; i++) {
            //    ChunkList *chunkList = parcArrayList_Get(_chunksByFileName, i);
            //    printf("In Movie List: [%d] %s\n", i, chunkList->fileName);
            //    if (strcmp(chunkList->fileName, fileName) == 0) {
            //        assertTrue(chunkList->chunks == chunkedContent, "WTF!");
            //    }
            //}

            parcMemory_Deallocate((void **) &fullFilePath);
        }
    }
        
    if (movieChunks != NULL) {

        if (requestedChunkNumber < parcArrayList_Size(movieChunks->chunks)) {
            result = parcArrayList_Get(movieChunks->chunks, requestedChunkNumber);
            //printf("Returning result: %p, for chunk #%ld, from chunklist %p / %p\n", 
            //       (void *)result, requestedChunkNumber, movieChunks, movieChunks->chunks);
        } else {
            printf("Requested out of range chunk %ld for %s. Returning NULL\n", requestedChunkNumber, movieChunks->fileName);
        }
    }

    return result; // Could be NULL if there was no payload
}

/**
 * Given a CCNxName, a directory path, and a requested chunk number, create a directory listing and return the specified
 * chunk of the directory listing as the payload of a newly created CCNxContentObject.
 * The new CCnxContentObject must eventually be released by calling ccnxContentObject_Release().
 *
 * @param [in] name The CCNxName to use when creating the new CCNxContentObject.
 * @param [in] directoryPath The directory whose contents are being listed.
 * @param [in] requestedChunkNumber The number of the requested chunk from the complete directory listing.
 *
 * @return A new CCNxContentObject instance containing the request chunk of the directory listing.
 */
static CCNxContentObject *
_createListResponse(CCNxName *name, const char *directoryPath, uint64_t requestedChunkNumber)
{
    CCNxContentObject *result = NULL;

    PARCBuffer *directoryList = tutorialFileIO_CreateDirectoryListing(directoryPath);

    uint64_t totalChunksInDirList = _getNumberOfChunksRequired(parcBuffer_Limit(directoryList), tutorialCommon_ChunkSize);
    if (requestedChunkNumber < totalChunksInDirList) {
        // Set the buffer's position to the start of the desired chunk.
        parcBuffer_SetPosition(directoryList, (requestedChunkNumber * tutorialCommon_ChunkSize));

        // See if we have more than 1 chunk's worth of data to in the buffer. If so, set the buffer's limit
        // to the end of the chunk.
        size_t chunkLen = parcBuffer_Remaining(directoryList);

        if (chunkLen > tutorialCommon_ChunkSize) {
            parcBuffer_SetLimit(directoryList, parcBuffer_Position(directoryList) + tutorialCommon_ChunkSize);
        }

        printf("tutorialServer: Responding to 'list' command with chunk %ld/%ld\n", (unsigned long) requestedChunkNumber, (unsigned long) totalChunksInDirList);

        // Calculate the final chunk number
        uint64_t finalChunkNumber = (totalChunksInDirList > 0) ? totalChunksInDirList - 1 : 0; // the final chunk, 0-based

        // At this point, dirListBuf has its position and limit set to the beginning and end of the
        // specified chunk.
        result = _createContentObject(name, directoryList, finalChunkNumber);
    }

    parcBuffer_Release(&directoryList);

    return result;
}

/**
 * Given a CCnxInterest that matched our domain prefix, see what the embedded command is and
 * create a corresponding CCNxContentObject as a response. The resulting CCNxContentObject
 * must eventually be released by calling ccnxContentObject_Release().
 *
 * @param [in] interest A CCNxInterest that matched the specified domain prefix.
 * @param [in] domainPrefix A CCNxName containing the domain prefix.
 * @param [in] directoryPath A string containing the path to the directory being served.
 *
 * @return A newly creatd CCNxContentObject contaning a response to the specified Interest,
 *         or NULL if the Interest couldn't be answered.
 */
static CCNxContentObject *
_createInterestResponse(const CCNxInterest *interest, const CCNxName *domainPrefix, const char *directoryPath, bool doPreChunk)
{
    CCNxName *interestName = ccnxInterest_GetName(interest);

    char *command = tutorialCommon_CreateCommandStringFromName(interestName, domainPrefix);

    uint64_t requestedChunkNumber = tutorialCommon_GetChunkNumberFromName(interestName);

    //char *interestNameString = ccnxName_ToString(interestName);
    //printf("tutorialServer: received Interest for chunk %d of %s, command = %s\n",
    //       (int) requestedChunkNumber, interestNameString, command);
    //parcMemory_Deallocate((void **) &interestNameString);

    CCNxContentObject *result = NULL;
    if (strncasecmp(command, tutorialCommon_CommandList, strlen(command)) == 0) {
        // This was a 'list' command. We should return the requested chunk of the directory listing.
        result = _createListResponse(interestName, directoryPath, requestedChunkNumber);
    } else if (strncasecmp(command, tutorialCommon_CommandFetch, strlen(command)) == 0) {
        // This was a 'fetch' command. We should return the requested chunk of the file specified.

        char *fileName = tutorialCommon_CreateFileNameFromName(interestName);
        if (doPreChunk) {
            result = _createFetchResponseWithPreChunking(interestName, directoryPath, fileName, requestedChunkNumber);
        } else {
            result = _createFetchResponse(interestName, directoryPath, fileName, requestedChunkNumber);
        }
        parcMemory_Deallocate((void **) &fileName);
    }

    parcMemory_Deallocate((void **) &command);

    return result;
}

/**
 * Listen for arriving Interests and respond to them if possible. We expect that the Portal we are passed is
 * listening for messages matching the specified domainPrefix.
 *
 * @param [in] portal The CCNxPortal that we will read from.
 * @param [in] domainPrefix A CCNxName containing the domain prefix that the specified `portal` is listening for.
 * @param [in] directoryPath A string containing the path to the directory being served.
 *
 * @return true if at least one Interest is received and responded to, false otherwise.
 */
static bool
_receiveAndAnswerInterests(CCNxPortal *portal, const CCNxName *domainPrefix, const char *directoryPath, bool doPreChunk)
{
    bool result = false;
    CCNxMetaMessage *inboundMessage = NULL;

    while ((inboundMessage = ccnxPortal_Receive(portal, CCNxStackTimeout_Never)) != NULL) {
        if (ccnxMetaMessage_IsInterest(inboundMessage)) {
            CCNxInterest *interest = ccnxMetaMessage_GetInterest(inboundMessage);
            CCNxContentObject *response = _createInterestResponse(interest, domainPrefix, directoryPath, doPreChunk);
            // At this point, response has either the requested chunk of the request file/command,
            // or remains NULL.

            if (response != NULL) {
                // We had a response, so send it back through the Portal.
                CCNxMetaMessage *responseMessage = ccnxMetaMessage_CreateFromContentObject(response);

                if (ccnxPortal_Send(portal, responseMessage, CCNxStackTimeout_Never) == false) {
                    fprintf(stderr, "ccnxPortal_Send failed (error %d). Is the Forwarder running?\n", ccnxPortal_GetError(portal));
                }

                ccnxMetaMessage_Release(&responseMessage);

                if (!doPreChunk) {
                    ccnxContentObject_Release(&response);
                }

                result = true; // We have received, and responded to, at least one Interest.
            }
        }
        ccnxMetaMessage_Release(&inboundMessage);
    }
    return result;
}

/**
 * Using the CCNxPortal API, listen for and respond to Interests matching our domain prefix (as defined in tutorial_Common.c).
 * The specified directoryPath is the location of the directory from which file and listing responses will originate.
 *
 * @param [in] directoryPath A string containing the path to the directory being served.
 *
 * @return true if at least one Interest is received and responded to, false otherwise.
 */
static bool
_serveDirectory(const char *directoryPath, bool doPreChunk)
{
    bool result = false;


    CCNxPortalFactory *factory = _setupServerPortalFactory();

    CCNxPortal *portal = ccnxPortalFactory_CreatePortal(factory, ccnxPortalRTA_Message);

    assertNotNull(portal, "Expected a non-null CCNxPortal pointer. Is the Forwarder running?");

    CCNxName *domainPrefix = ccnxName_CreateFromURI(tutorialCommon_DomainPrefix);

    if (ccnxPortal_Listen(portal, domainPrefix, 365 * 86400, CCNxStackTimeout_Never)) {
        printf("tutorial_Server: now serving files from %s\n", directoryPath);
        result = _receiveAndAnswerInterests(portal, domainPrefix, directoryPath, doPreChunk);
    }

    ccnxPortal_Release(&portal);
    ccnxPortalFactory_Release(&factory);

    return result;
}

/**
 * Display an explanation of arguments accepted by this program.
 *
 * @param [in] programName The name of this program.
 */
static void
_displayUsage(char *programName)
{
    printf("\n%s\n%s, %s\n\n", tutorialAbout_Version(), tutorialAbout_Name(), programName);

    printf(" This example file server application can provide access to files in the specified directory.\n");
    printf(" A CCNx forwarder (e.g. Metis) must be running before running it. Once running, the peer\n");
    printf(" tutorialClient application can request a listing or a specified file.\n\n");

    printf("Usage: %s [-l lci:/a/b/c] [-h] [-v] <directory path>\n", programName);
    printf("  '%s ~/files' will serve the files in ~/files\n", programName);
    printf("  '%s -v' will show the tutorial demo code version\n", programName);
    printf("  '%s -h' will show this help\n\n", programName);
}

int
main(int argc, char *argv[argc])
{
    int status = EXIT_FAILURE;

    char *commandArgs[argc];
    int commandArgCount = 0;
    bool needToShowUsage = false;
    bool shouldExit = false;

    status = tutorialCommon_processCommandLineArguments(argc, argv, &commandArgCount, commandArgs, &needToShowUsage, &shouldExit);

    if (needToShowUsage) {
        _displayUsage(argv[0]);
    }

    if (shouldExit) {
        exit(status);
    }

    _chunksByFileName = parcArrayList_Create(NULL);

    bool doPreChunk = true; // Set this to false if you don't want to load files into memory
                            // before responding to Interests

    if (commandArgCount == 1) {
        status = (_serveDirectory(commandArgs[0], doPreChunk) ? EXIT_SUCCESS : EXIT_FAILURE);
    } else {
        status = EXIT_FAILURE;
        _displayUsage(argv[0]);
    }

    for (int i = 0; i < parcArrayList_Size(_chunksByFileName); i++) {
        ChunkList *chunkList = parcArrayList_Get(_chunksByFileName, i);
        parcMemory_Deallocate((void **) &chunkList->fileName);
        parcArrayList_Destroy(&chunkList->chunks);
        parcMemory_Deallocate((void **) &chunkList);
    }

    parcArrayList_Destroy(&_chunksByFileName);

    exit(status);
}
