/*
 * File:        tcpy.c
 *
 * Author:      fossette
 *
 * Date:        2022/06/12
 *
 * Version:     3.0
 *
 * Description: Copy the content of the source (file or directory) into
 *              the destination.  The destination is created if it
 *              doesn't exist.  If the destination is omited, the
 *              current directory is used as the destination.  Operations
 *              on directories are recursive, i.e, sub-directories will
 *              also be copied.  Copy operations can be paused or stopped
 *              using the keyboard.  Copy operations are duration
 *              adjusted, i.e, should a write operation be slower, the
 *              next buffer will be given equivalent time for completion.
 *              Copy operations are verified afterward.
 *
 *              Accepted keyboard keys are:
 *                - ESC,   'Q' : Quit the copy process
 *                - SPACE, 'P' : Pause the copy process
 *                - 'V' :        Pause after the verify process
 *
 *              The -del parameter transform the COPY operation into
 *              a MOVE operation.  The source (file or directory) is
 *              deleted after a successful copy to the destination.
 *
 *              The -mir parameter is used to make the destination
 *              directory a mirror of the source directory.  Files of
 *              the destination directory will be deleted if they are
 *              no longer present in the source directory.
 *
 *              The -sync parameter considers both directories as
 *              masters.  Only the latest version of a file will be kept,
 *              then duplicated in the other directory.  If the status
 *              of a file can't be clearly determined, for example if
 *              the same file is modified in both directories, a backup
 *              copy will be created.  Note: Not Yet Implemented!
 *
 *              The -f parameter is used to disable the copy delay,
 *              "faster" mode.
 *
 *              The -t parameter activate the TEST RUN mode.
 *              Directories may be created, but no file will be copied
 *              nor deleted.
 *
 *              Tested under FreeBSD 12.2.  Should be easy to port
 *              because there are not that many dependencies.
 *
 * Parameters:  [-del|-mir] [-f] [-t] <src-file>|<src-dir> [<dest-file>|<dest-dir>]
 *
 * Web:         https://github.com/fossette/tcpy/wiki
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>




/*
 *  Constants
 */

// # define TCPY_DEBUG              1

#define ERROR_TCPY              1
#define ERROR_TCPY_USAGE        2
#define ERROR_TCPY_MEM          3
#define ERROR_TCPY_CIRC         4
#define ERROR_TCPY_STOP         5

#define COPYCOUNT                50
#define LNBIGBUFFER              32768
#define LNCHECKSUMBUFFER         1000
#define LNSZ                     300
#define ONESECINNANO             1000000000

#define TCPY_MODE_COPY          0
#define TCPY_MODE_DEL           1
#define TCPY_MODE_MIRROR        2
#define TCPY_MODE_SYNC          3




/*
 *  Types
 */

typedef unsigned long long TNSEC, *PTNSEC;




/*
 *  Global variable
 */

int     giFaster = 0,
        giFileCount = 0,
        giPauseAfterVerif = 0,
        giTestRun = 0;
char    *gpBigBuffer = NULL,
        gszErr[LNSZ];
TNSEC   giNanoFastest = 0,
        giNanoPrev = 0;
ssize_t giCopyByteCount = 0,
        giTotalByteCount = 0;

// Circular directory prevention!  No source directory can match this!
__dev_t  giSt_dev = 0;      /* inode's device */
ino_t    giSt_ino = 0;      /* inode's number */



///////////////////////////////////////////////////////////////////////////
//    Level 1 : General Purpose Functions                                //
///////////////////////////////////////////////////////////////////////////

/*
 *  ChecksumAdd
 *
 *  The initial seed value of checksums is 0.
 */

void
ChecksumAdd(char *pBigBuffer, ssize_t iSize, unsigned long *pChecksum)
{
   unsigned long iMsb;
   
   
   while (iSize)
   {
      iMsb = *pChecksum & 0x80000000;
      *pChecksum ^= ( ((unsigned long)(*pBigBuffer)) & 0xFF );
      *pChecksum <<= 1;
      if (iMsb)
         *pChecksum |= 1;

      iSize--;
      pBigBuffer++;
   }
}




/*
 *  EchoPrint
 */

void
EchoPrint(const char *pSz)
{
   printf("%s\n", pSz);
}




/*
 *  KeyboardCheck
 */

int
KeyboardCheck(int iInducedPause)
{
   int      i,
            iErr = 0,
            iPause;
   fd_set   sFdSet;
   struct timeval sTime = {0, 0};


   iPause = iInducedPause;
   if (iPause)
      EchoPrint("Pause...");

   do
   {
      FD_ZERO(&sFdSet);
      FD_SET(STDIN_FILENO, &sFdSet);
      if (select(STDIN_FILENO + 1, &sFdSet, NULL, NULL, &sTime) > 0)
      {
         i = getchar();
         if (feof(stdin) || ferror(stdin))
            clearerr(stdin);
         if (i == ' ' || i == 'p' || i == 'P')           // SPACE
         {
            iPause = !iPause;
            if (iPause)
               EchoPrint("Pause...");
            else
               EchoPrint("Resume...");
         }
         else if (i == 27 || i == 'q' || i == 'Q')       // ESC
            iErr = ERROR_TCPY_STOP;
         else if (i == 'v' || i == 'V')
         {
            giPauseAfterVerif = 1;
            EchoPrint("Pause Requested!");
         }

         if (iPause)
            usleep(3000);  // Pause 0.3 sec.
      }
   }
   while (!iErr && iPause);
   
   return(iErr);                            
}




/*
 *  NanoTime
 */

TNSEC
NanoTime(void)
{
   TNSEC iTime;
   struct timespec sTime;


   if (clock_gettime(CLOCK_REALTIME_FAST,     &sTime))
   {
      iTime = sTime.tv_sec * ONESECINNANO;
      iTime += sTime.tv_nsec;
   }
   else
      iTime = 0;
   
   return (iTime);
}




/*
 *  StringShortner
 */

void
StringShortner(const char *pLongSz, int iMax,     char *pShortSz)
{
   int i;


   i = strlen(pLongSz);
   if (i < iMax)
      strcpy(pShortSz, pLongSz);
   else
   {
      if (i <= iMax + 5)      // Borderline for the dots separator?
         iMax -= 5;

      iMax /= 2;
      strncpy(pShortSz, pLongSz, iMax);
      pShortSz[iMax] = 0;
      strcat(pShortSz, " ... ");
      strcat(pShortSz, pLongSz + i - iMax);
   }
}




///////////////////////////////////////////////////////////////////////////
//    Level 2 : Directory Functions                                      //
///////////////////////////////////////////////////////////////////////////

/*
 *  DirectoryExist
 */

int
DirectoryExist(const char *szPathname,     struct stat *pStat)
{
   int i;
   struct stat sStat, *pStat2;


   if (pStat)
      pStat2 = pStat;
   else
      pStat2 = &sStat;

   i = !stat(szPathname,     pStat2);
   if (i)
      i = (pStat2->st_mode & S_IFDIR);

#ifdef TCPY_DEBUG
   printf("\n%d = DirectoryExist(%s), st_mode=0%o\n", i, szPathname, (unsigned int)(pStat2->st_mode));
#endif // TCPY_DEBUG

   return(i);
}




/*
 *  DirectoryValidate
 */

int
DirectoryValidate(const char *szPathname,     struct stat *pStat)
{
   int   i,
         iErr = 0;
   char  sz[LNSZ],
         sz2[LNSZ],
         *pSzDirname;
   struct stat sStat, *pStat2;


   i = strlen(szPathname);
   if (i)
   {
      if (pStat)
         pStat2 = pStat;
      else
         pStat2 = &sStat;

      if (!DirectoryExist(szPathname,     pStat2))
      {
         pSzDirname = (char *)malloc(i + 10);
         if (pSzDirname)
         {
            strcpy(pSzDirname, szPathname);
            if (pSzDirname[i - 1] == '/')    // Slash the ending slash
            {
               i--;
               pSzDirname[i] = 0;
            }

            // Make sure that the parent directory exists
            while (i && pSzDirname[i] != '/')
               i--;
            if (pSzDirname[i] == '/')
            {
               if (i)
               {
                  pSzDirname[i] = 0;
                  iErr = DirectoryValidate(pSzDirname,     pStat2);
                  pSzDirname[i] = '/';
               }
               else
                  stat("/",     pStat2);
            }
            else
               stat(".",     pStat2);

            if (!iErr)
            {
               StringShortner(pSzDirname, LNSZ - 40,     sz);
               sprintf(sz2, "mkdir(%s, Mode=0%o)", sz,
                            (unsigned int)pStat2->st_mode);
               EchoPrint(sz2);
               if (mkdir(pSzDirname, pStat2->st_mode))
               {
                  iErr = ERROR_TCPY;
                  sprintf(gszErr, "Could Not Create %s (errno=%d)",
                                  sz, errno);
               }
               else
                  stat(pSzDirname,     pStat2);
            }
      
            free(pSzDirname);
         }
         else
            iErr = ERROR_TCPY_MEM;
      }
   }
   else if (pStat)
      // Not throwing an error but making sure there is no side effect
      memset(pStat, 0, sizeof(struct stat));

   return(iErr);                            
}




/*
 *  FilenameChecksum
 */

int
FilenameChecksum(const char *szFilename,     unsigned long *piChecksum)
{
   int      iErr = 0,
            iFd;
   ssize_t  iRead;
   char     sz[LNSZ];


   *piChecksum = 0;
   iFd = open(szFilename, O_RDONLY);
   if (iFd < 0)
   {
      iErr = ERROR_TCPY;
      StringShortner(szFilename, LNSZ - 50,     sz);
      sprintf(gszErr, "Could Not Open %s (errno=%d)", sz, errno);
   }
   if (!iErr)
   {
      do
      {
         iRead = read(iFd, gpBigBuffer, LNBIGBUFFER);
         if (iRead > 0)
            ChecksumAdd(gpBigBuffer, iRead, piChecksum);

         iErr = KeyboardCheck(0);
      }
      while (iRead == LNBIGBUFFER && !iErr);

      close(iFd);
   }
   
   return(iErr);                            
}




/*
 *  FilenameExist
 */

int
FilenameExist(const char *szPathname,     struct stat *pStat)
{
   int i;
   struct stat sStat, *pStat2;


   if (pStat)
      pStat2 = pStat;
   else
      pStat2 = &sStat;

   i = !stat(szPathname,     pStat2);
   if (i)
      i = (pStat2->st_mode & S_IFREG);

#ifdef TCPY_DEBUG
   printf("\n%d = FilenameExist(%s), st_mode=0%o\n", i, szPathname, (unsigned int)(pStat2->st_mode));
#endif // TCPY_DEBUG

   return(i);
}




///////////////////////////////////////////////////////////////////////////
//    Level 3 : Sub-systems                                              //
///////////////////////////////////////////////////////////////////////////

/*
 *  TimedCopyFile
 */

int
TimedCopyFile(const int iMode, const char *szSourceFilename,
              const char *szDestFilename)
{
   int               iErr = 0,
                     iFdDest = -1,
                     iFdSource = -1,
                     iExistDest,
                     iExistSource;
   unsigned long     iDestChecksum = 0,
                     iSourceChecksum = 0;
   ssize_t           iRead,
                     iWrite;
   TNSEC             iNano;
   char              sz2[LNSZ],
                     szDest[LNSZ],
                     szSource[LNSZ];
   struct stat       sStatDest,
                     sStatSource;
   struct timespec   sTimes[2];


   StringShortner(szSourceFilename, LNSZ - 80,     szSource);
   StringShortner(szDestFilename, LNSZ - 80,     szDest);

   // Verify existing source and destination
   iExistSource = FilenameExist(szSourceFilename,     &sStatSource);
   if (!iExistSource)
   {
      iErr = ERROR_TCPY;
      sprintf(gszErr, "File %s Not Found!", szSource);
   }
   iExistDest = FilenameExist(szDestFilename,     &sStatDest);
   if (!iExistDest)
   {
      sStatDest.st_size = 0;
#if defined(_WANT_FREEBSD11_STAT)
      sStatDest.st_birthtim.tv_sec = 0;
      sStatDest.st_birthtim.tv_nsec = 0;
#endif
      sStatDest.st_mtim.tv_sec = 0;
      sStatDest.st_mtim.tv_nsec = 0;
   }

   if (!iErr && sStatSource.st_size && sStatDest.st_size
       && sStatSource.st_size == sStatDest.st_size)
   {
      sprintf(sz2, "Verify %s to %s", szSource, szDest);
      EchoPrint(sz2);
      if (!giTestRun)
      {
         iErr = FilenameChecksum(szSourceFilename,     &iSourceChecksum);
         if (!iErr)
            iErr = FilenameChecksum(szDestFilename,     &iDestChecksum);
      }
   }

   if (!iErr && (sStatSource.st_size != sStatDest.st_size
                 || sStatSource.st_mtim.tv_sec
                    != sStatDest.st_mtim.tv_sec
                 || sStatSource.st_mtim.tv_nsec
                    != sStatDest.st_mtim.tv_nsec
                 || iSourceChecksum != iDestChecksum))
   {
      if (iExistDest)
      {
         sprintf(sz2, "Delete %s (diff", szDest);
         if (sStatSource.st_size != sStatDest.st_size)
            sprintf(sz2+strlen(sz2), " %ld bytes",
                    sStatDest.st_size - sStatSource.st_size);
         if (sStatSource.st_mtim.tv_sec != sStatDest.st_mtim.tv_sec)
            strcat(sz2, " sec");
         if (sStatSource.st_mtim.tv_nsec != sStatDest.st_mtim.tv_nsec)
            strcat(sz2, " nsec");
         if (iSourceChecksum != iDestChecksum)
            strcat(sz2, " chk");
         strcat(sz2, ")");
         EchoPrint(sz2);
         if (!giTestRun)
            if (unlink(szDestFilename))
            {
               iErr = ERROR_TCPY;
               sprintf(gszErr, "Could Not Delete %s (errno=%d)",
                               szDest, errno);
            }
      }
      if (!iErr)
      {
         // Copy Operation
         sprintf(sz2, "Copy %s to %s", szSource, szDest);
         EchoPrint(sz2);
         iDestChecksum = 0;
         if (!giTestRun)
         {
            iFdSource = open(szSourceFilename, O_RDONLY);
            if (iFdSource < 0)
            {
               iErr = ERROR_TCPY;
               sprintf(gszErr, "Could Not Open %s (errno=%d)",
                               szSource, errno);
            }
            if (!iErr)
            {
               iFdDest = open(szDestFilename, O_WRONLY|O_CREAT|O_TRUNC,
                              sStatSource.st_mode);
               if (iFdDest < 0)
               {
                  iErr = ERROR_TCPY;
                  sprintf(gszErr, "Could Not Create %s (errno=%d)",
                                  szDest, errno);
               }
            }
            if (!iErr)
               do
               {
                  iRead = read(iFdSource, gpBigBuffer, LNBIGBUFFER);
                  if (iRead > 0)
                  {
                     giCopyByteCount += iRead;
                     giTotalByteCount += iRead;

                     // Slowdown for next write if needed
                     iNano = giNanoPrev - giNanoFastest;
                     if (iRead != LNBIGBUFFER)
                        iNano = (iNano * iRead) / LNBIGBUFFER;
                     sTimes[0].tv_sec = iNano / ONESECINNANO;
                     sTimes[0].tv_nsec = iNano % ONESECINNANO;
                     if (!giFaster)
                        nanosleep(sTimes, NULL);

                     ChecksumAdd(gpBigBuffer, iRead,     &iDestChecksum);

                     iNano = NanoTime();
                     iWrite = write(iFdDest, gpBigBuffer, iRead);
                     giNanoPrev = NanoTime() - iNano;
                     if (iRead != LNBIGBUFFER)
                        giNanoPrev = (giNanoPrev * LNBIGBUFFER) / iRead;
                     if (!giNanoFastest || giNanoPrev < giNanoFastest)
                        giNanoFastest = giNanoPrev;

                     if (iWrite != iRead)
                     {
                        iErr = ERROR_TCPY;
                        sprintf(gszErr, "Write to file %s Failed (errno=%d)",
                                        szDest, errno);
                     }
                  }
                  if (!iErr)
                     iErr = KeyboardCheck(0);
               }
               while (iRead == LNBIGBUFFER && !iErr);

            if (iFdDest >= 0)
               close(iFdDest);
            if (iFdSource >= 0)
               close(iFdSource);

            if (!iErr)
            {
               if (iSourceChecksum)
               {
                  if (iSourceChecksum != iDestChecksum)
                  {
                     iErr = ERROR_TCPY;
                     sprintf(gszErr, "Source %s Check Failed!", szSource);
                  }
               }
               else
                  iSourceChecksum = iDestChecksum;
            }
            if (iErr)
            {
               if (unlink(szDestFilename))
                  printf("\nWARNING: Failed to delete %s (errno=%d)\n",
                         szDest, errno);
            }

            // Adjust creation and modification times
            if (!iErr)
            {
               sTimes[0].tv_sec = UTIME_OMIT;
               sTimes[0].tv_nsec = UTIME_OMIT;
#if defined(_WANT_FREEBSD11_STAT)
               sTimes[1].tv_sec = sStatSource.st_birthtim.tv_sec;
               sTimes[1].tv_nsec = sStatSource.st_birthtim.tv_nsec;
               if (utimensat(AT_FDCWD, szDestFilename, sTimes, 0))
               {
                  iErr = ERROR_TCPY;
                  sprintf(gszErr, "Time Set of %s Failed!", szSource);
               }
#endif
            }
            if (!iErr)
            {
               sTimes[1].tv_sec = sStatSource.st_mtim.tv_sec;
               sTimes[1].tv_nsec = sStatSource.st_mtim.tv_nsec;
               if (utimensat(AT_FDCWD, szDestFilename, sTimes, 0))
               {
                  iErr = ERROR_TCPY;
                  sprintf(gszErr, "Time Set of %s Failed!", szSource);
               }
            }
         }
      }

      if (!iErr)
      {
         // Verify Destination Operation
         sprintf(sz2, "Verify %s", szDest);
         EchoPrint(sz2);
         if (!giTestRun)
         {
            iErr = FilenameChecksum(szDestFilename,     &iDestChecksum);
            if (!iErr && iSourceChecksum != iDestChecksum)
            {
               iErr = ERROR_TCPY;
               sprintf(gszErr, "Destination %s Check Failed!", szDest);
               if (unlink(szDestFilename))
                  printf("\nWARNING: Failed to delete %s (errno=%d)\n",
                         szDest, errno);
            }
         }
      }
   }

   if (!iErr && iMode == TCPY_MODE_DEL)
   {
      // Delete Source Operation
      sprintf(sz2, "Delete %s", szSource);
      EchoPrint(sz2);
      if (!giTestRun)
         if (unlink(szSourceFilename))
         {
            iErr = ERROR_TCPY;
            sprintf(gszErr, "Failed to delete %s (errno=%d)",
                            szDest, errno);
         }
   }
   
   if (!iErr)
   {
      giFileCount++;
      if (giPauseAfterVerif)
      {
         giCopyByteCount = 0;
         giFileCount = 0;
         giPauseAfterVerif = 0;
         iErr = KeyboardCheck(1);
      }
      else if (giFileCount >= COPYCOUNT && !giFaster)
      {
         sprintf(sz2, "%d files done, 10 sec. Pause...", COPYCOUNT);
         EchoPrint(sz2);
         usleep(10000000);
         giFileCount = 0;
      }
      else if (giCopyByteCount > 1073741824)
      {
         giCopyByteCount *= 30;
         giCopyByteCount /= 1024;
         if (giFaster)
            sprintf(sz2, "%d Gb done.",
                    (int)(giTotalByteCount/1073741824));
         else
            sprintf(sz2, "%d Gb done, %d sec. Pause...",
                    (int)(giTotalByteCount/1073741824),
                    (int)(giCopyByteCount/1000000));
         EchoPrint(sz2);
         if (!giFaster)
            usleep(giCopyByteCount);
         giCopyByteCount = 0;
         giFileCount = 0;
      }
   }

   return(iErr);
}




/*
 *  TimedCopy
 */

int
TimedCopy(const int iMode,
          const char *szSourceDir, const char *szSourceFile,
          const char *szDestDir,   const char *szDestFile)
{
   int   i,
         iErr = 0;
   char  sz[LNSZ],
         sz2[LNSZ],
         *pSzFilenameDest = NULL,
         *pSzFilenameSource = NULL;
   DIR   *pDir;
   struct dirent  *pDirEntry;
   struct stat    sStat;


#ifdef TCPY_DEBUG
   printf("\nMode: %d\nFrom: %s%s\nTo:   %s%s\n", iMode,
          szSourceDir, szSourceFile, szDestDir, szDestFile);
#endif // TCPY_DEBUG

   i = LNSZ;
   if (i < strlen(szSourceFile))
      i = strlen(szSourceFile);
   if (i < strlen(szDestFile))
      i = strlen(szDestFile);
   i += 10;
   pSzFilenameSource = (char *)malloc(strlen(szSourceDir) + i);
   pSzFilenameDest = (char *)malloc(strlen(szDestDir) + i);
   if (!(pSzFilenameSource && pSzFilenameDest))
      iErr = ERROR_TCPY_MEM;

   if (!iErr)
   {
      if (strcmp(szSourceDir, szDestDir))
      {
         // Different directories
         if (DirectoryExist(szSourceDir,     &sStat))
         {
            if (sStat.st_dev == giSt_dev && sStat.st_ino == giSt_ino)
               iErr = ERROR_TCPY_CIRC;
         }
         else
            iErr = ERROR_TCPY_USAGE;

         if (!iErr)
         {
            if (DirectoryExist(szDestDir,     &sStat))
            {
               if (!(giSt_dev || giSt_ino))
               {
                  giSt_dev = sStat.st_dev;
                  giSt_ino = sStat.st_ino;
               }
            }
            else
               iErr = ERROR_TCPY_USAGE;
         }
         if (!iErr)
         {
            if (*szSourceFile)
            {
               strcpy(pSzFilenameSource, szSourceDir);
               strcat(pSzFilenameSource, szSourceFile);
               strcpy(pSzFilenameDest, szDestDir);
               strcat(pSzFilenameDest, szDestFile);
               iErr = TimedCopyFile(iMode, pSzFilenameSource,
                                           pSzFilenameDest);
            }
            else
            {
               pDir = opendir(szSourceDir);
               if (pDir)
               {
                  do
                  {
                     pDirEntry = readdir(pDir);
                     if (pDirEntry)
                     {
                        if (strcmp(pDirEntry->d_name, ".")
                            && strcmp(pDirEntry->d_name, ".."))
                        {
                           if (strlen(pDirEntry->d_name) > LNSZ)
                           {
                              iErr = ERROR_TCPY;
                              StringShortner(pDirEntry->d_name, LNSZ - 40,
                                                                        sz);
                              sprintf(gszErr, "Name %s Too Long!", sz);
                           }
                           else if ((pDirEntry->d_type & DT_DIR) == DT_DIR)
                           {
                              strcpy(pSzFilenameSource, szSourceDir);
                              strcat(pSzFilenameSource, pDirEntry->d_name);
                              i = strlen(pSzFilenameSource);
                              if (pSzFilenameSource[i - 1] != '/')
                              {
                                 pSzFilenameSource[i] = '/';
                                 pSzFilenameSource[i + 1] = 0;
                              }
                              strcpy(pSzFilenameDest, szDestDir);
                              strcat(pSzFilenameDest, pDirEntry->d_name);
                              i = strlen(pSzFilenameDest);
                              if (pSzFilenameDest[i - 1] != '/')
                              {
                                 pSzFilenameDest[i] = '/';
                                 pSzFilenameDest[i + 1] = 0;
                              }

                              iErr = DirectoryValidate(pSzFilenameDest,
                                                                     NULL);
                              if (!iErr)
                                 iErr = TimedCopy(iMode,
                                                  pSzFilenameSource, "",
                                                  pSzFilenameDest, "");
                           }
                           else if ((pDirEntry->d_type & DT_REG) == DT_REG)
                           {
                              strcpy(pSzFilenameSource, szSourceDir);
                              strcat(pSzFilenameSource, pDirEntry->d_name);
                              strcpy(pSzFilenameDest, szDestDir);
                              strcat(pSzFilenameDest, pDirEntry->d_name);
                              iErr = TimedCopyFile(iMode, pSzFilenameSource,
                                                          pSzFilenameDest);
                           }
                        }
                     }
                  }
                  while (pDirEntry && !iErr);

                  closedir(pDir);
               }

               // Mirror cleanup
               if (!iErr && iMode == TCPY_MODE_MIRROR && !(*szSourceFile))
               {
                  pDir = opendir(szDestDir);
                  if (pDir)
                  {
                     do
                     {
                        pDirEntry = readdir(pDir);
                        if (pDirEntry)
                           if ((pDirEntry->d_type & DT_REG) == DT_REG)
                           {
                              strcpy(pSzFilenameSource, szSourceDir);
                              strcat(pSzFilenameSource, pDirEntry->d_name);
                              strcpy(pSzFilenameDest, szDestDir);
                              strcat(pSzFilenameDest, pDirEntry->d_name);
                              if (!FilenameExist(pSzFilenameSource,   NULL))
                              {
                                 StringShortner(pSzFilenameDest, LNSZ - 30,
                                                                        sz);
                                 sprintf(sz2, "Delete %s", sz);
                                 EchoPrint(sz2);
                                 if (!giTestRun)
                                    if (unlink(pSzFilenameDest))
                                       printf("\nWARNING: Failed"
                                              " to delete %s\n", sz);
                              }
                           }
                     }
                     while (pDirEntry);

                     closedir(pDir);
                  }
               }
            }
         }
      }
      else if (*szSourceFile)
      {
         // Same directory
         if (strcmp(szSourceFile, szDestFile))
         {
            strcpy(pSzFilenameSource, szSourceDir);
            strcat(pSzFilenameSource, szSourceFile);
            strcpy(pSzFilenameDest, szDestDir);
            strcat(pSzFilenameDest, szDestFile);
            iErr = TimedCopyFile(iMode, pSzFilenameSource,
                                        pSzFilenameDest);
         }
         else
         {
            iErr = ERROR_TCPY;
            StringShortner(szSourceFile, LNSZ - 60,     sz);
            sprintf(gszErr, "Can't copy the %s file on itself!", sz);
         }
      }
      else
      {
         iErr = ERROR_TCPY;
         StringShortner(szSourceDir, LNSZ - 60,     sz);
         sprintf(gszErr, "Can't copy the %s directory on itself!", sz);
      }
   }
   
   if (pSzFilenameDest)
      free(pSzFilenameDest);
   if (pSzFilenameSource)
      free(pSzFilenameSource);
   
   return(iErr);
}




///////////////////////////////////////////////////////////////////////////
//    Level 4 : Main Program                                             //
///////////////////////////////////////////////////////////////////////////

/*
 *  main
 */

int
main(int argc, char* argv[])
{
   int      i,
            iErr = 0,
            iLn = LNSZ,
            iMode = TCPY_MODE_COPY,
            iOldStdinFlag,
            j;
   tcflag_t iOldLocalMode;
   char     *pDestDir = NULL,
            *pDestFile = NULL,
            *pSourceDir = NULL,
            *pSourceFile = NULL,
            *pSz;
   struct termios sTermios;

 
   *gszErr = 0;

   // Switch the stdin line behavior to INSTANT, NoEcho
   //    Not all functions do what their doc pretends.  This is a big
   //    mess just to have a simple non-blocking getchar() without
   //    curses nor threads.  If this doesn't work as is on your
   //    platform, at least you have a few hints to explore.
   setvbuf(stdin, NULL, _IONBF, 0);
   i = iOldStdinFlag = fcntl(STDIN_FILENO, F_GETFL);
   i &= ~O_NONBLOCK;
   fcntl(STDIN_FILENO, F_SETFL, i);
   tcgetattr(STDIN_FILENO,     &sTermios);
   iOldLocalMode = sTermios.c_lflag;
   sTermios.c_lflag &= ~(ICANON|ECHO);
   tcsetattr(STDIN_FILENO, TCSANOW, &sTermios);
   
   // Make sure the path buffers are big enough
   i = argc;
   while (i)
   {
      i--;
      j = strlen(argv[i]);
      if (j > iLn)
         iLn = j;
   }
   iLn += 10;

   // Parse the command parameters
   if (argc < 2 || argc > 6)
      iErr = ERROR_TCPY_USAGE;

   for (i = 1 ; i < argc && !iErr ; i++)
   {
      if (!strcmp(argv[i], "-del"))
      {
         if (iMode)
            iErr = ERROR_TCPY_USAGE;
         else
            iMode = TCPY_MODE_DEL;
      }
      else if (!strcmp(argv[i], "-mir"))
      {
         if (iMode)
            iErr = ERROR_TCPY_USAGE;
         else
            iMode = TCPY_MODE_MIRROR;
      }
      else if (!strcmp(argv[i], "-sync"))
      {
         if (iMode)
            iErr = ERROR_TCPY_USAGE;
         else
         {
            //iMode = TCPY_MODE_SYNC;
            iErr = ERROR_TCPY;
            strcpy(gszErr, "Not Yet Implemented!");
         }
      }
      else if (!strcmp(argv[i], "-f"))
         giFaster = 1;
      else if (!strcmp(argv[i], "-t"))
         giTestRun = 1;
      else if (pSourceDir)
      {
         // Destination parameter
         if (FilenameExist(argv[i],     NULL))
         {
            if (*pSourceFile)
            {
               pDestDir = (char *)malloc(iLn);
               pDestFile = (char *)malloc(iLn);
               if (!(pDestDir && pDestFile))
                  iErr = ERROR_TCPY_MEM;
               if (!iErr)
               {
                  pSz = argv[i];
                  j = strlen(pSz);
                  while (j && pSz[j] != '/')
                     j--;
                  if (pSz[j] == '/')
                  {
                     strcpy(pDestDir, pSz);
                     pDestDir[j + 1] = 0;
                     strcpy(pDestFile, pSz + j + 1);
                  }
                  else
                  {
                     strcpy(pDestDir, "./");
                     strcpy(pDestFile, pSz);
                  }
               }
            }
            else
               iErr = ERROR_TCPY_USAGE;
         }
         else if (DirectoryExist(argv[i],     NULL))
         {
            pDestDir = (char *)malloc(iLn);
            pDestFile = (char *)malloc(iLn);
            if (!(pDestDir && pDestFile))
               iErr = ERROR_TCPY_MEM;
            if (!iErr)
            {
               strcpy(pDestDir, argv[i]);
               j = strlen(pDestDir);
               if (j)
               {
                  if (pDestDir[j - 1] != '/')
                  {
                     pDestDir[j] = '/';
                     pDestDir[j + 1] = 0;
                  }
               }
               *pDestFile = 0;
            }
         }
         else
         {
            pSz = argv[i];
            if (*pSz == '-')
               iErr = ERROR_TCPY_USAGE;
            if (!iErr)
            {
               pDestDir = (char *)malloc(iLn);
               pDestFile = (char *)malloc(iLn);
               if (!(pDestDir && pDestFile))
                  iErr = ERROR_TCPY_MEM;
            }
            if (!iErr)
            {
               if (*pSourceFile)
               {
                  j = strlen(pSz);
                  while (j && pSz[j] != '/')
                     j--;
                  if (pSz[j] == '/')
                  {
                     strcpy(pDestDir, pSz);
                     pDestDir[j + 1] = 0;
                     strcpy(pDestFile, pSz + j + 1);
                     iErr = DirectoryValidate(pDestDir,     NULL);
                  }
                  else
                  {
                     strcpy(pDestDir, "./");
                     strcpy(pDestFile, pSz);
                  }
               }
               else
               {
                  strcpy(pDestDir, pSz);
                  j = strlen(pDestDir);
                  if (j)
                  {
                     if (pDestDir[j - 1] != '/')
                     {
                        pDestDir[j] = '/';
                        pDestDir[j + 1] = 0;
                     }
                  }
                  *pDestFile = 0;
                  iErr = DirectoryValidate(pDestDir,     NULL);
               }
            }
         }
      }
      else
      {
         // Source parameter (must exist)
         if (FilenameExist(argv[i],     NULL))
         {
            pSourceDir = (char *)malloc(iLn);
            pSourceFile = (char *)malloc(iLn);
            if (!(pSourceDir && pSourceFile))
               iErr = ERROR_TCPY_MEM;
            if (!iErr)
            {
               pSz = argv[i];
               j = strlen(pSz);
               while (j && pSz[j] != '/')
                  j--;
               if (pSz[j] == '/')
               {
                  strcpy(pSourceDir, pSz);
                  pSourceDir[j + 1] = 0;
                  strcpy(pSourceFile, pSz + j + 1);
               }
               else
               {
                  strcpy(pSourceDir, "./");
                  strcpy(pSourceFile, pSz);
               }
            }
         }
         else if (DirectoryExist(argv[i],     NULL))
         {
            pSourceDir = (char *)malloc(iLn);
            pSourceFile = (char *)malloc(iLn);
            if (!(pSourceDir && pSourceFile))
               iErr = ERROR_TCPY_MEM;
            if (!iErr)
            {
               strcpy(pSourceDir, argv[i]);
               j = strlen(pSourceDir);
               if (j)
               {
                  if (pSourceDir[j - 1] != '/')
                  {
                     pSourceDir[j] = '/';
                     pSourceDir[j + 1] = 0;
                  }
               }
               *pSourceFile = 0;
            }
         }
         else
            iErr = ERROR_TCPY_USAGE;
      }
   }

   // Parameters parsing done
   if (!iErr)
   {
      // The source must exist at this point
      if (pSourceDir)
      {
         // If a file is specified, the mode can't be MIRROR or SYNC
         if (*pSourceFile && iMode > TCPY_MODE_DEL)
            iErr = ERROR_TCPY_USAGE;
      }
      else
         iErr = ERROR_TCPY_USAGE;
   }
   if (!iErr)
   {
      // If it hasn't been set already, set the destination
      // to the current directory
      if (!pDestDir)
      {
         pDestDir = (char *)malloc(iLn);
         pDestFile = (char *)malloc(iLn);
         if (!(pDestDir && pDestFile))
            iErr = ERROR_TCPY_MEM;
         if (!iErr)
         {
            strcpy(pDestDir, "./");
            *pDestFile = 0;
         }
      }
   }
   if (!iErr)
   {
      // If the destination filename hasn't been specified,
      // keep the same filename
      if (*pSourceFile && !(*pDestFile))
         strcpy(pDestFile, pSourceFile);
      
      gpBigBuffer = (char *)malloc(LNBIGBUFFER);
      if (!gpBigBuffer)
         iErr = ERROR_TCPY_MEM;
   }
   if (!iErr)
   {
      if (giTestRun)
         printf("\n*** TEST RUN ***\n");

      iErr = TimedCopy(iMode, pSourceDir, pSourceFile,
                              pDestDir,   pDestFile);
   }

   EchoPrint("");
   switch (iErr)
   {
      case 0:
         printf("Done!\n");
         break;

      case ERROR_TCPY:
         printf("ERROR: %s\n", gszErr);
         break;

      case ERROR_TCPY_USAGE:
         printf("USAGE: tcpy [-del|-mir] [-f] [-t]"
                " <src-file>|<src-dir> [<dest-file>|<dest-dir>]\n");
         break;

      case ERROR_TCPY_MEM:
         printf("ERROR: Out Of Memory!\n");
         break;

      case ERROR_TCPY_CIRC:
         printf("ERROR: Circular Directory Copy Atempted!\n");
         break;

      case ERROR_TCPY_STOP:
         printf("WARNING: Terminated by the user!\n");
         break;

      default:
         printf("ERROR: Unexpected Code %d\n", iErr);
   }

   if (gpBigBuffer)
      free(gpBigBuffer);

   if (pDestDir)
      free(pDestDir);
   if (pDestFile)
      free(pDestFile);
   if (pSourceDir)
      free(pSourceDir);
   if (pSourceFile)
      free(pSourceFile);

   // Exit gracefully with undos
   sTermios.c_lflag = iOldLocalMode;
   tcsetattr(STDIN_FILENO, TCSANOW, &sTermios);
   fcntl(STDIN_FILENO, F_SETFL, iOldStdinFlag);
   setlinebuf(stdin);
   
   return(0);
}
