/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.    Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s):
 * Mitesh Shah <mitesh@netscape.com>
 *
 */

#include "nsAutoConfig.h"
#include "nsIURI.h"
#include "nsIHttpChannel.h"
#include "nsIFileStreams.h"
#include "nsDirectoryServiceDefs.h"
#include "prmem.h"
#include "jsapi.h"
#include "prefapi.h"
#include "nsIProfile.h"
#include "nsIObserverService.h"
#include "nsIEventQueueService.h"
#include "nsLiteralString.h"


NS_IMPL_THREADSAFE_ISUPPORTS4(nsAutoConfig, nsIAutoConfig, nsITimerCallback, nsIStreamListener,nsIObserver)
    
nsAutoConfig::nsAutoConfig()
{
    /* member initializers and constructor code */
    NS_INIT_REFCNT();
}

nsresult nsAutoConfig::Init()
{
    /* member initializers and constructor code */

    nsresult rv=NS_OK;
    
    mLoaded = PR_FALSE;
    
    // Registering the object as an observer to the profile-after-change topic

    nsCOMPtr<nsIObserverService> observerService =
        do_GetService(NS_OBSERVERSERVICE_CONTRACTID, &rv);
    if (NS_FAILED(rv)) return rv;
    
    if (observerService) {
        rv = observerService->AddObserver(this, 
                                          NS_LITERAL_STRING("profile-after-change").get());
        if (NS_FAILED(rv)) return rv;
    }
    nsCOMPtr<nsIPrefService> prefs =
        do_GetService(NS_PREFSERVICE_CONTRACTID, &rv);
    if (NS_FAILED(rv)) return rv;
    rv = prefs->GetBranch(nsnull,getter_AddRefs(mPrefBranch));
    if (NS_FAILED(rv)) return rv;
    return NS_OK;
}

nsAutoConfig::~nsAutoConfig()
{
    nsresult rv;
    nsCOMPtr<nsIObserverService> observerService =
        do_GetService(NS_OBSERVERSERVICE_CONTRACTID, &rv);
    if (observerService) 
        rv = observerService->RemoveObserver(this, 
                                             NS_LITERAL_STRING("profile-after-change").get());
    
}


NS_IMETHODIMP
nsAutoConfig::OnStartRequest(nsIRequest* request, nsISupports* context)
{
    return NS_OK;
}


NS_IMETHODIMP
nsAutoConfig::OnDataAvailable(nsIRequest* request, 
                              nsISupports* context,
                              nsIInputStream *aIStream, 
                              PRUint32 aSourceOffset,
                              PRUint32 aLength)
{    
    PRUint32 amt, size;
    nsresult rv;
    char buf[1025];
    
    while (aLength) {
        size = PR_MIN(aLength, sizeof(buf));
        rv = aIStream->Read(buf, size, &amt);
        if (NS_FAILED(rv)) {
            NS_ASSERTION((NS_BASE_STREAM_WOULD_BLOCK != rv), 
                         "The stream should never block.");
            return rv;
        }
        mBuf.Append(buf,amt);
        aLength -= amt;
    }
    return NS_OK;
}


NS_IMETHODIMP
nsAutoConfig::OnStopRequest(nsIRequest* request, nsISupports* context,
                            nsresult aStatus)
{
    
    nsresult rv;

    // If the request is failed, go read the failover.jsc file
    if (NS_FAILED(aStatus)) {
        return readOfflineFile();
    }

    //Checking for the http response, if failure go read the failover file.
    nsCOMPtr<nsIHttpChannel> pHTTPCon(do_QueryInterface(request));
    if (pHTTPCon) {
        PRUint32 httpStatus;
        pHTTPCon->GetResponseStatus(&httpStatus);
        if (httpStatus != 200) 
            return readOfflineFile();
    }
    
    // Send the autoconfig.jsc to javascript engine.
    PRBool success = PREF_EvaluateConfigScript(mBuf.get(), mBuf.Length(),nsnull,
                                               PR_FALSE,PR_TRUE,PR_FALSE);
    if (success) {
        rv = writeFailoverFile(); /*  Write the autoconfig.jsc to 
                                      failover.jsc (cached copy) */
        if(NS_FAILED(rv)) 
            NS_WARNING("Error writing failover.jsc file");
        // Clean up the previous read. 
        // If there is a timer, these methods will be called again.
        mBuf.Truncate(0);
        mLoaded = PR_TRUE;  //Releasing the deadlock
        return NS_OK;
    }
    else {
        NS_WARNING("Error reading autoconfig.jsc from the network, reading the offline version");
        // Clean up the previous read so it will be ready for 
        // the next updated read.
        mBuf.Truncate(0); 
        return readOfflineFile();
    }
}

// Notify method as a TimerCallBack function 
NS_IMETHODIMP_(void) nsAutoConfig::Notify(nsITimer *timer) 
{
    DownloadAutoCfg();
}

/* Observe() is called twice: once at the instantiation time and other 
   after the profile is set. It doesn't do anything but return NS_OK during the
   creation time. Second time it calls  DownloadAutoCfg().
*/
NS_IMETHODIMP nsAutoConfig::Observe(nsISupports *aSubject, const PRUnichar *aTopic, const PRUnichar *someData)
{
    nsresult rv = NS_OK;
    if (!nsCRT::strcmp(aTopic, NS_LITERAL_STRING("profile-after-change").get()))
        {
            // Getting the current profile name since we already have the 
            // pointer to the object.
            nsCOMPtr<nsIProfile> profile = do_QueryInterface(aSubject);
            if (profile) {
                nsXPIDLString profileName;
                rv = profile->GetCurrentProfile(getter_Copies(profileName));
                if (NS_SUCCEEDED(rv)) 
                    // setting the member variable to the current profile name
                    mCurrProfile.AssignWithConversion(profileName); 
                else 
                    return rv;
            }
            return DownloadAutoCfg();
        }  
    else if (!nsCRT::strcmp(aTopic, NS_LITERAL_STRING("app-startup").get())) 
        {
            // This is the object instantiation, do nothing and return NS_OK;
            return NS_OK;
        }
    return NS_OK;
}

NS_IMETHODIMP nsAutoConfig::DownloadAutoCfg()
{
    nsresult rv = NS_OK;
    nsCAutoString emailAddr;
    nsXPIDLCString urlName;
    PRBool appendMail=PR_FALSE, offline=PR_FALSE;
    static PRBool firstTime = PR_TRUE;
    
    
    // get the value of the autoconfig url
    rv = mPrefBranch->GetCharPref("autoadmin.global_config_url",
                                 getter_Copies(urlName));
    if(NS_FAILED(rv) || (nsCRT::strlen(urlName) == 0))  
        return NS_OK; /* Return ok if there is no config url set. */

    // Check to see if the network is online/offline 
    
    nsCOMPtr<nsIIOService> ios =
        do_GetService(NS_IOSERVICE_CONTRACTID, &rv);
    if (NS_FAILED(rv)) return rv;
    
    rv = ios->GetOffline(&offline);
    if (NS_FAILED(rv)) return rv;
    
    if (offline) {
        
        PRBool offlineFailover = PR_FALSE;
        rv = mPrefBranch->GetBoolPref("autoadmin.offline_failover", 
                                     &offlineFailover);
        
        // Read the failover.jsc if the network is offline and the pref says so
        if ( offlineFailover ) {
            return readOfflineFile();
        }
    }
    
    
    
    nsCAutoString cfgUrl(urlName);
    
    /* Append user's identity at the end of the URL if the pref says so.
       First we are checking for the user's email address but if it is not
       available in the case where the client is used without messenger, user's 
       profile name will be used as an unique identifier
    */
    
    rv = mPrefBranch->GetBoolPref("autoadmin.append_emailaddr", &appendMail);
    
    if (NS_SUCCEEDED(rv) && appendMail) {
        rv = getEmailAddr(emailAddr);
        if (NS_SUCCEEDED(rv) && emailAddr) {
            /* Adding the unique identifier at the end of autoconfig URL. 
               In this case the autoconfig URL is a script and 
               emailAddr as passed as an argument */
            cfgUrl.Append("?");
            cfgUrl.Append(emailAddr); 
        }
    }
    
    // Getting an event queue. If we start an AsyncOpen, the thread
    // needs to wait before the reading of autoconfig is done
    
    nsCOMPtr<nsIEventQueue> currentThreadQ;

    if (firstTime) {
        nsCOMPtr<nsIEventQueueService> service = 
            do_GetService(NS_EVENTQUEUESERVICE_CONTRACTID, &rv);
        if (NS_FAILED(rv)) return rv;
        rv = service->GetThreadEventQueue(NS_CURRENT_THREAD,getter_AddRefs(currentThreadQ));
        NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);
    }
    mLoaded = PR_FALSE;
    
    // create a new url 
    
    nsCOMPtr<nsIURI> url;
    nsCOMPtr<nsIChannel> channel;
    
    rv = NS_NewURI(getter_AddRefs(url), cfgUrl, nsnull, nsnull);
    if(NS_FAILED(rv)) return rv;
    
    // open a channel for the url
    
    rv = NS_OpenURI(getter_AddRefs(channel),url, nsnull, nsnull);
    if(NS_FAILED(rv)) return rv;
    
    rv = channel->AsyncOpen(this, nsnull); 
    if (NS_FAILED(rv)) {
        readOfflineFile();
        return rv;
    }
    
    
    // Set a repeating timer if the pref is set.
    // This is to be done only once.
    
    if (firstTime) {
        firstTime = PR_FALSE;
        PRInt32 minutes = 0;
        rv = mPrefBranch->GetIntPref("autoadmin.refresh_interval", 
                                    &minutes);
        if(NS_SUCCEEDED(rv) && minutes > 0) {
            // Create a new timer and pass this nsAutoConfig 
            // object as a timer callback. 
            nsCOMPtr<nsITimer> timer;
            timer = do_CreateInstance("@mozilla.org/timer;1",&rv);
            if (NS_FAILED(rv)) return rv;
            rv = timer->Init(this, minutes*60*1000, NS_PRIORITY_NORMAL, 
                        NS_TYPE_REPEATING_SLACK);
            if (NS_FAILED(rv)) return rv;
        }

    
    
        // process events until we're finished. AutoConfig.jsc reading needs
        // to be finished before the browser starts loading up
        // We are waiting for the mLoaded which will be set through 
        // onStopRequest or readOfflineFile methods
        // There is a possibility of deadlock so we need to make sure
        // that mLoaded will be set to true in any case (success/failure)
        
        PLEvent *event;
        while (!mLoaded) {
            rv = currentThreadQ->WaitForEvent(&event);
            NS_ASSERTION(NS_SUCCEEDED(rv),"-->nsAutoConfig::DownloadAutoCfg: currentThreadQ->WaitForEvent failed...");
            if (NS_FAILED(rv)) return rv;
            rv = currentThreadQ->HandleEvent(event);
            NS_ASSERTION(NS_SUCCEEDED(rv), "-->nsAutoConfig::DownloadAutoCfg: currentThreadQ->HandleEvent failed...");
            if (NS_FAILED(rv)) return rv;
            
        }
        
    } //first_time
    
    return NS_OK;

} // nsPref::DownloadAutoCfg()



nsresult nsAutoConfig::readOfflineFile()
{
    PRBool failCache = PR_TRUE;
    nsresult rv;
    PRBool offline;
    
    rv = mPrefBranch->GetBoolPref("autoadmin.failover_to_cached", &failCache);
    
    if (failCache == PR_FALSE) {
        
        // disable network connections and return.
        
        nsCOMPtr<nsIIOService> ios =
            do_GetService(NS_IOSERVICE_CONTRACTID, &rv);
        if (NS_FAILED(rv)) return reportError();
        
        rv = ios->GetOffline(&offline);
        if (NS_FAILED(rv)) return reportError();
        if (!offline) {
            rv = ios->SetOffline(PR_TRUE);
            if (NS_FAILED(rv)) return reportError();
        }
        
        // lock the "network.online" prference so user cannot toggle back to
        // online mode.
        rv = mPrefBranch->SetBoolPref("network.online", PR_FALSE);
        if (NS_FAILED(rv)) return reportError();
        mPrefBranch->LockPref("network.online");

        mLoaded = PR_TRUE;
        return NS_OK;

    }
    
    // failCache = true 
    
    // Open the file and read the content.
    // execute the javascript file
    
    nsCOMPtr<nsIFile> failoverFile; 
    rv = NS_GetSpecialDirectory(NS_XPCOM_CURRENT_PROCESS_DIR, 
                                getter_AddRefs(failoverFile));
    if (NS_FAILED(rv)) return reportError();
#ifdef XP_MAC
    failoverFile->Append("Essential Files");
#endif
    
    failoverFile->Append("failover.jsc");
    rv = evaluateLocalFile(failoverFile);
    if (NS_FAILED(rv)) 
        NS_WARNING("Couldn't open failover.jsc, going back to default prefs");
    mLoaded = PR_TRUE;
    return NS_OK;
}

nsresult nsAutoConfig::evaluateLocalFile(nsIFile* file)
{
    nsresult rv;
    nsCOMPtr<nsIInputStream> inStr;
    
    rv = NS_NewLocalFileInputStream(getter_AddRefs(inStr), file);
    if (NS_FAILED(rv)) return rv;
        
    PRInt64 fileSize;
    PRUint32 fs, amt=0;
    file->GetFileSize(&fileSize);
    LL_L2UI(fs, fileSize); // Converting 64 bit structure to unsigned int
    char*  buf = (char *) PR_Malloc(fs*sizeof(char)) ;
    if(!buf) 
        return NS_ERROR_OUT_OF_MEMORY;
    
    rv = inStr->Read(buf, fs, &amt);
    if (NS_FAILED(rv)) 
        NS_ASSERTION((NS_BASE_STREAM_WOULD_BLOCK != rv), 
                     "The stream should never block.");
    
    if (!PREF_EvaluateConfigScript(buf,fs,nsnull, PR_FALSE, PR_TRUE, PR_FALSE)){
        PR_FREEIF(buf);
        return NS_ERROR_FAILURE;
    }
    inStr->Close();
    PR_FREEIF(buf);
    return NS_OK;
}

nsresult nsAutoConfig::writeFailoverFile()
{
    nsresult rv;
    nsCOMPtr<nsIFile> failoverFile; 
    nsCOMPtr<nsIOutputStream> outStr;
    PRUint32 amt;

    rv = NS_GetSpecialDirectory(NS_XPCOM_CURRENT_PROCESS_DIR, 
                                getter_AddRefs(failoverFile));
    if (NS_FAILED(rv)) return rv;
#ifdef XP_MAC
    failoverFile->Append("Essential Files");
#endif
    
    failoverFile->Append("failover.jsc");
    
    rv = NS_NewLocalFileOutputStream(getter_AddRefs(outStr), failoverFile);
    if (NS_FAILED(rv)) return rv;
    rv = outStr->Write(mBuf.get(),mBuf.Length(),&amt);
    outStr->Close();
    return rv;
}

nsresult nsAutoConfig::getEmailAddr(nsAWritableCString & emailAddr)
{

    nsresult rv;
    nsXPIDLCString prefValue;
    
    /* Getting an email address through set of three preferences:
       First getting a default account with 
       "mail.accountmanager.defaultaccount"
       second getting an associated id with the default account
       Third getting an email address with id
    */
    
    rv = mPrefBranch->GetCharPref("mail.accountmanager.defaultaccount", 
                                  getter_Copies(prefValue));
    // Checking prefValue and its length.  Since by default the preference 
    // is set to nothing
    PRUint32 len;
    if (NS_SUCCEEDED(rv) && (len = nsCRT::strlen(prefValue)) > 0) {
        emailAddr = NS_LITERAL_CSTRING("mail.account.") + 
            nsDependentCString(prefValue, len) + NS_LITERAL_CSTRING(".identities");
        rv = mPrefBranch->GetCharPref(PromiseFlatCString(emailAddr).get(),
                                      getter_Copies(prefValue));

          // should the following |strlen|s ask |== 0|?  |PRUint32| can't be |<0|
        if (NS_FAILED(rv) || (len = nsCRT::strlen(prefValue)) < 0) return rv;
        emailAddr = NS_LITERAL_CSTRING("mail.identity.") + 
            nsDependentCString(prefValue, len) + NS_LITERAL_CSTRING(".useremail");
        rv = mPrefBranch->GetCharPref(PromiseFlatCString(emailAddr).get(),
                                      getter_Copies(prefValue));
        if (NS_FAILED(rv)  || (len = nsCRT::strlen(prefValue)) < 0) return rv;
        emailAddr = nsDependentCString(prefValue, len);
    }
    else {
        if (mCurrProfile) {
            emailAddr = mCurrProfile;
        }
    }
    
    return NS_OK;
}
        
 
nsresult nsAutoConfig::reportError()
{
    NS_ERROR("AutoConfig::readOfflineFile() failed");
    mLoaded = PR_TRUE; //Releasing lock to avoid deadlock in DownloadAutoCfg()
    return NS_ERROR_FAILURE;
}
