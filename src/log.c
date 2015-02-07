/*
 * Copyright (c) 2014, Sureshkumar Nedunchezhian.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of prez nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "prez.h"
#include "cluster.h"
#include "endianconv.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

/* Log Utilities */

listNode *getLogNode(long long index) {
    listNode *n;
    n = listIndex(server.cluster->log_entries,(index-1-logLength));
    return n;
}

logEntryNode *getLogEntry(long long index) {
    listNode *n;
    logEntryNode *en=NULL;
    n = getLogNode(index);
    if (n) en = listNodeValue(n);
    return en;
}

/* Log file loading */

/* Load the log file and read the log entries 
 * */
int loadLogFile(void) {
    FILE *fp;
    struct prez_stat sb;

    server.cluster->log_fd = open(server.cluster->log_filename,
            O_WRONLY|O_APPEND|O_CREAT,0644);
    if (server.cluster->log_fd == -1) {
        prezLog(PREZ_WARNING,"Prez can't open the log file: %s",
                strerror(errno));
        return PREZ_ERR;
    }
    fp = fopen(server.cluster->log_filename, "r");
    if (fp && prez_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.cluster->log_current_size = 0;
        fclose(fp);
        prezLog(PREZ_NOTICE,"Prez log empty");
        return PREZ_OK;
    }

    if (fp == NULL) {
        prezLog(PREZ_WARNING,"Fatal error: "
                "can't open log file for reading: %s",strerror(errno));
        exit(1);
    }

    server.cluster->log_current_size = 0;

    while(1) {
        int argc, j, ok;
        unsigned long len;
        char buf[128];
        sds argsds;
        logEntryNode *entry;
        size_t loglen;

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;
        loglen = strlen(buf);

        entry = zmalloc(sizeof(*entry));
        for (j = 0; j < argc; j++) {
            if (fgets(buf,sizeof(buf),fp) == NULL) goto readerr;
            if (buf[0] != '$') goto fmterr;
            len = strtol(buf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            if (len && fread(argsds,len,1,fp) == 0) goto fmterr;
            loglen += (len+strlen(buf));
            switch(j) {
                case LOG_TYPE_INDEX:
                    ok = string2ll(argsds,len,&entry->log_entry.index);
                    if (!ok) goto readerr;
                    break;
                case LOG_TYPE_TERM:
                    ok = string2ll(argsds,len,&entry->log_entry.term);
                    if (!ok) goto readerr;
                    break;
                case LOG_TYPE_COMMANDNAME:
                    memcpy(entry->log_entry.commandName, argsds,
                            sizeof(entry->log_entry.commandName));
                    break;
                case LOG_TYPE_COMMAND:
                    memcpy(entry->log_entry.command, argsds,
                            sizeof(entry->log_entry.command));
                    break;
                default:
                    goto fmterr;
                    break;
            }
            if (fread(buf,2,1,fp) == 0) goto fmterr; /* discard CRLF */
            loglen  += 2;
        }
        entry->position = server.cluster->log_current_size;
        server.cluster->log_current_size += loglen;

        listAddNodeTail(server.cluster->log_entries, entry);
    }

    fclose(fp);
    return PREZ_OK;

readerr:
    if (feof(fp)) {
        prezLog(PREZ_WARNING,
                "Unexpected end of file reading the prez log file");
    } else {
        prezLog(PREZ_WARNING,
                "Unrecoverable error reading the prez log file: %s",
                strerror(errno));
    }
    exit(1);
fmterr:
    prezLog(PREZ_WARNING,"Bad file format reading the prez log file");
    exit(1);
}

int logVerifyAppend(long long index, long long term) {
    logEntryNode *entry;

    if (!index) return PREZ_OK;
    if (index > logLength) {
        prezLog(PREZ_NOTICE, "Index doesn't exist. length:%lu "
                "index:%lld term:%lld",
                logLength, index, term);
        return PREZ_ERR;
    }

    entry = getLogEntry(index);
    if (entry->log_entry.term != term) {
        prezLog(PREZ_NOTICE, "logVerifyAppend: Entry at index doesn't match term. "
                "index %lld term %lld",
                index, term);
        return PREZ_ERR;
    }
    return PREZ_OK;
}

int logTruncate(long long index) {
    logEntryNode *entry;
    listNode *ln;
    listIter li;

    if (index < logLength) {
        entry = getLogEntry(index);
        ftruncate(server.cluster->log_fd, entry->position);
        server.cluster->log_current_size = entry->position;
        listRewind(server.cluster->log_entries, &li);
        li.next = getLogNode(index);
        while ((ln = listNext(&li)) != NULL) {
            logEntryNode *le = listNodeValue(ln);
            listDelNode(server.cluster->log_entries,ln);
            zfree(le);
        }
    }
    return PREZ_OK;
}

sds catLogEntry(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}

int logWriteEntry(logEntry e) {
    ssize_t nwritten;
    robj *argv[4];
    sds buf = sdsempty();
    logEntryNode *en;

    if (logLength > 0) {
        en = getLogEntry(e.index);
        if (en) {
            if (en->log_entry.index == e.index && en->log_entry.term == e.term) {
                return PREZ_OK;
            } else if (e.term != en->log_entry.term &&
                    e.index == en->log_entry.index) {
                prezLog(PREZ_NOTICE, "Conflict detected, truncate"
                        "new term:%lld index:%lld, last term:%lld",
                        e.term, e.index, en->log_entry.term);
                logTruncate(e.index);
            }
        }
    }

    /* Persist to log */
    argv[0] = createStringObjectFromLongLong(e.index);
    argv[1] = createStringObjectFromLongLong(e.term);
    argv[2] = createStringObject(e.commandName,strlen(e.commandName));
    argv[3] = createStringObject(e.command,strlen(e.command));
    buf = catLogEntry(buf, 4, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
    decrRefCount(argv[3]);
    nwritten = write(server.cluster->log_fd,buf,sdslen(buf));
    if (nwritten != (signed)sdslen(buf)) {
        prezLog(PREZ_NOTICE,"log write incomplete");
    }

    /* Append to log list */
    en = zmalloc(sizeof(*en));
    memset(en,0,sizeof(*en));
    en->log_entry.index = e.index;
    en->log_entry.term = e.term;
    memcpy(en->log_entry.commandName, e.commandName,strlen(e.commandName));
    memcpy(en->log_entry.command, e.command,strlen(e.command));
    en->position = server.cluster->log_current_size;
    server.cluster->log_current_size += sdslen(buf);
    prezLog(PREZ_DEBUG,"logWriteEntry: term:%lld/%lld index:%lld len:%lu",
            en->log_entry.term,
            e.term,
            en->log_entry.index,
            logLength);
    listAddNodeTail(server.cluster->log_entries,en);

    return PREZ_OK;
}

int logAppendEntries(clusterMsgDataAppendEntries entries) {
    int i=0;
    logEntry *e;

    e = (logEntry*) entries.log_entries;
    for(i=0;i<ntohs(entries.log_entries_count);i++) {
        if(logWriteEntry(entries.log_entries[i])) {
            prezLog(PREZ_NOTICE, "log write error");
            return PREZ_ERR;
        }
        e++;
    }
    if(fsync(server.cluster->log_fd) == -1) return PREZ_ERR;
    return PREZ_OK;
}

int logCommitIndex(long long leader_commit_index) {
    if (leader_commit_index > server.cluster->commit_index) {
        server.cluster->commit_index =
            ((leader_commit_index < logCurrentIndex()) ? leader_commit_index :
             logCurrentIndex());
    }

    return PREZ_OK;
}

int logApply(long long index) {
    int i,argc;
    sds *argv;
    robj **oargv;
    struct prezCommand *cmd;
    prezClient *c;

    logEntryNode *entry = getLogEntry(index);

    if (!entry) return PREZ_OK;

    /* Process command which is what commit is really about */
    if (server.cluster->state == PREZ_LEADER &&
            (c = dictFetchValue(server.cluster->proc_clients,
                                sdsfromlonglong(index)))) {
        call(c);
        dictDelete(server.cluster->proc_clients,
                sdsfromlonglong(index));
        return PREZ_OK;
    }
    argv = sdssplitargs(entry->log_entry.command,&argc);
    if (argv == NULL) return PREZ_OK;
    oargv = zmalloc(sizeof(robj*)*argc);
    for(i=0;i<argc;i++) {
        if (sdslen(argv[i])) {
            oargv[i] = createObject(PREZ_STRING,argv[i]);
        } else {
            sdsfree(argv[i]);
        }
    }
    zfree(argv);
    cmd = lookupCommand(oargv[0]->ptr);
    if (!cmd) {
        prezLog(PREZ_NOTICE,"unknown command '%s'",
                (char*)oargv[0]->ptr);
        return PREZ_OK;
    } else if ((cmd->arity > 0 && cmd->arity != argc) ||
            (argc < -cmd->arity)) {
        prezLog(PREZ_NOTICE,"wrong number of arguments for '%s' command",
                cmd->name);
        return PREZ_OK;
    }
    cmd->proc(NULL,oargv,argc);
    for(i=0;i<argc;i++) decrRefCount(oargv[i]);
    return PREZ_OK;
}

int logSync(void) {
    if(fsync(server.cluster->log_fd) == -1) return PREZ_ERR;
    return PREZ_OK;
}

long long logCurrentIndex(void) {
    listNode *ln;
    logEntryNode *entry;

    ln = listIndex(server.cluster->log_entries, -1);
    if (ln) {
        entry = ln->value;
        return(entry->log_entry.index);
    }
    return 0;
}

long long logCurrentTerm(void) {
    listNode *ln;
    logEntryNode *entry;

    ln = listIndex(server.cluster->log_entries, -1);
    if (ln) {
        entry = ln->value;
        return(entry->log_entry.term);
    }
    return 0;
}

long long logGetTerm(long long index) {
    listNode *ln;
    logEntryNode *entry;

    ln = getLogNode(index);
    if (ln) {
        entry = ln->value;
        return(entry->log_entry.term);
    }
    return 0;
}
