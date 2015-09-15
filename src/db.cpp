/**
 * @file db.cpp
 * @brief Database access interface
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/db.h"
#include "mega/utils.h"
#include "mega/base64.h"
#include "mega/logging.h"

namespace mega {
DbTable::DbTable(SymmCipher *key)
{
    this->key = key;
    this->hkey = NULL;
    this->phkey = NULL;
}

DbTable::~DbTable()
{
    delete hkey;
    delete phkey;
}

bool DbTable::putrootnodes(handle *rootnodes)
{
    string data;

    for (int i=0; i<3; i++)
    {
        encrypthandle(rootnodes[i], &data);

        if (!putrootnode(i+1, &data))    // 0: scsn 1-3: rootnodes
        {
            return false;
        }
    }

    return true;
}

bool DbTable::getrootnodes(handle *rootnodes)
{
    string data;

    for (int i=0; i<3; i++)
    {
        if (!getrootnode(i+1, &data))    // 0: scsn 1-3: rootnodes
        {
            return false;
        }

        decrypthandle(&rootnodes[i], &data);
    }

    return true;
}

bool DbTable::putnode(pnode_t n)
{
    string data;
    n->serialize(&data);
    PaddedCBC::encrypt(&data, key);

    handle h = n->nodehandle;
    SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);

    handle ph = n->parenthandle;
    SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);

    string fp;
    if(n->type == FILENODE)
    {
        n->serializefingerprint(&fp);
        PaddedCBC::encrypt(&fp, key);
    }

    int shared = 0;
    if (n->outshares)
    {
        shared = 1;
    }
    if (n->inshare)
    {
        shared = 2;
    }
    if (n->pendingshares)
    {
        shared += 3;
        // A node may have outshares and pending shares at the same time (value=4)
        // A node cannot be an inshare and a pending share at the same time
    }

    bool result = putnode(h, ph, &fp, n->attrstring, shared, &data);

    if(!result)
    {
        LOG_err << "Error recording node " << n->nodehandle;
    }

    return result;
}

bool DbTable::putuser(User * u)
{
    if (ISUNDEF(u->userhandle))
    {
        LOG_debug << "Skipping the recording of a non-existing user";
        // The SDK creates a User during share's creation, even if the target email is not a contact yet
        // The User should not be written into DB as a user, but as a pending contact

        return true;
    }

    string data;
    u->serialize(&data);
    PaddedCBC::encrypt(&data, key);

    handle userhandle = u->userhandle;
    SymmCipher::xorblock((byte*)&userhandle, hkey, HANDLEKEYLENGTH);

    return putuser(userhandle, &data);
}

bool DbTable::putpcr(PendingContactRequest *pcr)
{
    string data;
    pcr->serialize(&data);
    PaddedCBC::encrypt(&data, key);

    handle id = pcr->id;
    SymmCipher::xorblock((byte*)&id, hkey, HANDLEKEYLENGTH);

    return putpcr(id, &data);
}

bool DbTable::delnode(pnode_t n)
{
    handle h = n->nodehandle;
    SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);

    return delnode(h);
}

bool DbTable::delpcr(PendingContactRequest *pcr)
{
    handle id = pcr->id;
    SymmCipher::xorblock((byte*)&id, hkey, HANDLEKEYLENGTH);

    return delpcr(id);
}

bool DbTable::getnode(handle h, string* data)
{
    SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);

    if (getnodebyhandle(h, data))
    {
        return PaddedCBC::decrypt(data, key);
    }

    return false;
}

bool DbTable::getnode(string *fingerprint, string* data)
{
    PaddedCBC::encrypt(fingerprint, key);

    if (getnodebyfingerprint(fingerprint, data))
    {
        return PaddedCBC::decrypt(data, key);
    }

    return false;
}

bool DbTable::getuser(string* data)
{
    if (next(data))
    {
        return PaddedCBC::decrypt(data, key);
    }

    return false;
}

bool DbTable::getpcr(string *data)
{
    if (next(data))
    {
        return PaddedCBC::decrypt(data, key);
    }

    return false;
}

bool DbTable::getnumchildren(handle ph, int *count)
{
    SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);

    return getnumchildrenquery(ph, count);
}

bool DbTable::getnumchildfiles(handle ph, int *count)
{
    SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);

    return getnumchildfilesquery(ph, count);
}

bool DbTable::getnumchildfolders(handle ph, int *count)
{
    SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);

    return getnumchildfoldersquery(ph, count);
}

handle_vector * DbTable::gethandleschildren(handle ph)
{
    handle_vector *hchildren = new handle_vector;

    SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);
    rewindhandleschildren(ph);

    handle h;
    while (nexthandle(&h))
    {
        SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);
        hchildren->push_back(h);
    }

    return hchildren;
}

handle_vector *DbTable::gethandlesencryptednodes()
{
    handle_vector *hencryptednodes = new handle_vector;

    rewindhandlesencryptednodes();

    handle h;
    while (nexthandle(&h))
    {
        SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);
        hencryptednodes->push_back(h);
    }

    return hencryptednodes;
}

// if 'h' is defined, get only the outshares that are child nodes of 'h'
handle_vector *DbTable::gethandlesoutshares(handle ph)
{
    handle_vector *hshares = new handle_vector;

    if (ph != UNDEF)
    {
        SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);
        rewindhandlesoutshares(ph);
    }
    else
    {
        rewindhandlesoutshares();
    }

    handle h;
    while (nexthandle(&h))
    {
        SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);
        hshares->push_back(h);
    }

    return hshares;
}

// if 'h' is defined, get only the pending shares that are child nodes of 'h'
handle_vector *DbTable::gethandlespendingshares(handle ph)
{
    handle_vector *hshares = new handle_vector;

    if (ph != UNDEF)
    {
        SymmCipher::xorblock((byte*)&ph, phkey, HANDLEKEYLENGTH);
        rewindhandlespendingshares(ph);
    }
    else
    {
        rewindhandlespendingshares();
    }

    handle h;
    while (nexthandle(&h))
    {
        SymmCipher::xorblock((byte*)&h, hkey, HANDLEKEYLENGTH);
        hshares->push_back(h);
    }

    return hshares;
}

void DbTable::encrypthandle(handle h, string *hstring)
{
    hstring->resize(sizeof(handle) * 4/3 + 3);
    hstring->resize(Base64::btoa((const byte *)&h, sizeof(handle), (char *) hstring->data()));

    PaddedCBC::encrypt(hstring, key);
}

void DbTable::decrypthandle(handle *h, string *hstring)
{
    if (PaddedCBC::decrypt(hstring, key))
    {
        Base64::atob(hstring->data(), (byte *)h, hstring->size());
    }
}

// add or update record with padding and encryption
bool DbTable::put(uint32_t type, Cachable* record, SymmCipher* key)
{
    string data;

    if (!record->serialize(&data))
    {
        //Don't return false if there are errors in the serialization
        //to let the SDK continue and save the rest of records
        return true;
    }

    PaddedCBC::encrypt(&data, key);

    if (!record->dbid)
    {
        record->dbid = (nextid += IDSPACING) | type;
    }

    return put(record->dbid, (char*)data.data(), data.size());
}

// get next record, decrypt and unpad
bool DbTable::next(uint32_t* type, string* data, SymmCipher* key)
{
    if (next(type, data))
    {
        if (!*type)
        {
            return true;
        }

        if (*type > nextid)
        {
            nextid = *type & - IDSPACING;
        }

        return PaddedCBC::decrypt(data, key);
    }

    return false;
}


DbQuery::DbQuery(DbTable *sctable, QueryType type)
{
    this->sctable = sctable;
    this->type = type;

    this->h = UNDEF;
    this->number = 0;
}

void DbQuery::execute()
{
    if (!sctable)
    {
        err = API_ENOENT;
        return;
    }

    switch (type)
    {
    case GET_NUM_CHILD_FILES:
        err = sctable->getnumchildfiles(h, &number) ? API_OK : API_EREAD;
        break;

    case GET_NUM_CHILD_FOLDERS:
        err = sctable->getnumchildfolders(h, &number) ? API_OK : API_EREAD;
        break;

    default:
        LOG_warn << "Execution of unknown type of DbQuery";
        err = API_EARGS;
        break;
    }
}

DbQueryQueue::DbQueryQueue()
{
    mutex.init(false);
}

bool DbQueryQueue::empty()
{
    mutex.lock();

    bool result = dbqueries.empty();

    mutex.unlock();

    return result;
}

void DbQueryQueue::push(DbQuery *query)
{
    mutex.lock();

    dbqueries.push_back(query);

    mutex.unlock();
}

DbQuery * DbQueryQueue::front()
{
    mutex.lock();

    DbQuery *result = dbqueries.front();

    mutex.unlock();

    return result;
}

void DbQueryQueue::pop()
{
    mutex.lock();

    dbqueries.pop_front();

    mutex.unlock();
}

void * DbThread::loop(void *param)
{
    MegaClient *client = (MegaClient *)param;

    int r;
    DbQuery *query;
    bool threadExit = false;

    while (true)
    {
        client->dbwaiter->init(Waiter::ds);
        r = client->dbwaiter->wait();
        if (r & Waiter::NEEDEXEC)
        {
            // execute all the queued queries
            while (!client->dbqueryqueue.empty())
            {
                query = client->dbqueryqueue.front();
                query->execute();

                // return the result to the MegaApi layer by calling the corresponding callback
                switch (query->type)
                {
                case DbQuery::GET_NUM_CHILD_FILES:
                    client->app->getnumchildfiles_result(query->getNumber(), query->getError());
                    break;

                case DbQuery::GET_NUM_CHILD_FOLDERS:
                    client->app->getnumchildfolders_result(query->getNumber(), query->getError());
                    break;

                case DbQuery::DELETE:
                    threadExit = true;
                    break;
                }

                client->dbqueryqueue.pop();
            }

            if (threadExit)
                break;
        }
    }
}

} // namespace
