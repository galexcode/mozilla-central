/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2001
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Scott MacGregor <scott@scott-macgregor.org>
 *   Dan Mosedale <dmose@mozillamessaging.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nsMsgContentPolicy.h"
#include "nsIServiceManager.h"
#include "nsIDocShellTreeItem.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch2.h"
#include "nsIURI.h"
#include "nsCOMPtr.h"
#include "nsIMsgHeaderParser.h"
#include "nsIAbManager.h"
#include "nsIAbDirectory.h"
#include "nsIAbCard.h"
#include "nsIMsgMailNewsUrl.h"
#include "nsIMsgWindow.h"
#include "nsIMimeMiscStatus.h"
#include "nsIMsgMessageService.h"
#include "nsIMsgIncomingServer.h"
#include "nsIRssIncomingServer.h"
#include "nsIMsgHdr.h"
#include "nsMsgUtils.h"

#include "nsIMsgComposeService.h"
#include "nsIMsgCompose.h"
#include "nsMsgCompCID.h"

// needed by the content load policy manager
#include "nsIExternalProtocolService.h"
#include "nsCExternalHandlerService.h"

// needed for the cookie policy manager
#include "nsNetUtil.h"
#include "nsICookie2.h"

// needed for mailnews content load policy manager
#include "nsIDocShell.h"
#include "nsIWebNavigation.h"
#include "nsIDocShellTreeNode.h"
#include "nsContentPolicyUtils.h"
#include "nsIDOMHTMLImageElement.h"
#include "nsILoadContext.h"
#include "nsIFrameLoader.h"
#include "nsIWebProgress.h"

static const char kBlockRemoteImages[] = "mailnews.message_display.disable_remote_image";
static const char kAllowPlugins[] = "mailnews.message_display.allow.plugins";
static const char kTrustedDomains[] =  "mail.trusteddomains";

// Per message headder flags to keep track of whether the user is allowing remote
// content for a particular message. 
// if you change or add more values to these constants, be sure to modify
// the corresponding definitions in mailWindowOverlay.js
#define kNoRemoteContentPolicy 0
#define kBlockRemoteContent 1
#define kAllowRemoteContent 2

NS_IMPL_ISUPPORTS4(nsMsgContentPolicy, 
                   nsIContentPolicy,
                   nsIWebProgressListener,
                   nsIObserver,
                   nsISupportsWeakReference)

nsMsgContentPolicy::nsMsgContentPolicy()
{
  mAllowPlugins = PR_FALSE;
  mBlockRemoteImages = PR_TRUE;
}

nsMsgContentPolicy::~nsMsgContentPolicy()
{
  // hey, we are going away...clean up after ourself....unregister our observer
  nsresult rv;
  nsCOMPtr<nsIPrefBranch2> prefInternal = do_GetService(NS_PREFSERVICE_CONTRACTID, &rv);
  if (NS_SUCCEEDED(rv))
  {
    prefInternal->RemoveObserver(kBlockRemoteImages, this);
    prefInternal->RemoveObserver(kAllowPlugins, this);
  }
}

nsresult nsMsgContentPolicy::Init()
{
  nsresult rv;

  // register ourself as an observer on the mail preference to block remote images
  nsCOMPtr<nsIPrefBranch2> prefInternal = do_GetService(NS_PREFSERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  prefInternal->AddObserver(kBlockRemoteImages, this, PR_TRUE);
  prefInternal->AddObserver(kAllowPlugins, this, PR_TRUE);

  prefInternal->GetBoolPref(kAllowPlugins, &mAllowPlugins);
  prefInternal->GetCharPref(kTrustedDomains, getter_Copies(mTrustedMailDomains));
  prefInternal->GetBoolPref(kBlockRemoteImages, &mBlockRemoteImages);

  return NS_OK;
}

/** 
 * returns true if the sender referenced by aMsgHdr is in one one of our local
 * address books and the user has explicitly allowed remote content for the sender
 */
nsresult nsMsgContentPolicy::AllowRemoteContentForSender(nsIMsgDBHdr * aMsgHdr, PRBool * aAllowForSender)
{
  NS_ENSURE_ARG_POINTER(aMsgHdr); 
  
  nsresult rv;
  *aAllowForSender = PR_FALSE;  

  // extract the e-mail address from the msg hdr
  nsCString author;
  rv = aMsgHdr->GetAuthor(getter_Copies(author));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgHeaderParser> headerParser = do_GetService("@mozilla.org/messenger/headerparser;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCString emailAddress; 
  rv = headerParser->ExtractHeaderAddressMailboxes(author, emailAddress);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIAbManager> abManager = do_GetService("@mozilla.org/abmanager;1",
                                                   &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsISimpleEnumerator> enumerator;
  rv = abManager->GetDirectories(getter_AddRefs(enumerator));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsISupports> supports;
  nsCOMPtr<nsIAbDirectory> directory;
  nsCOMPtr<nsIAbCard> cardForAddress;
  PRBool hasMore;

  while (NS_SUCCEEDED(enumerator->HasMoreElements(&hasMore)) && hasMore && !cardForAddress)
  {
    rv = enumerator->GetNext(getter_AddRefs(supports));
    NS_ENSURE_SUCCESS(rv, rv);
    directory = do_QueryInterface(supports);
    if (directory)
    {
      rv = directory->CardForEmailAddress(emailAddress, getter_AddRefs(cardForAddress));
      if (NS_FAILED(rv) && rv != NS_ERROR_NOT_IMPLEMENTED)
        return rv;
    }
  }
  
  // if we found a card from the sender, 
  if (cardForAddress)
    cardForAddress->GetPropertyAsBool(kAllowRemoteContentProperty, aAllowForSender);

  return NS_OK;
}

/**
 * Extract the host name from aContentLocation, and look it up in our list
 * of trusted domains.
 */
PRBool nsMsgContentPolicy::IsTrustedDomain(nsIURI * aContentLocation)
{
  PRBool trustedDomain = PR_FALSE;
  // get the host name of the server hosting the remote image
  nsCAutoString host;
  nsresult rv = aContentLocation->GetHost(host);

  if (NS_SUCCEEDED(rv) && !mTrustedMailDomains.IsEmpty()) 
    trustedDomain = MsgHostDomainIsTrusted(host, mTrustedMailDomains);

  return trustedDomain;
}


NS_IMETHODIMP
nsMsgContentPolicy::ShouldLoad(PRUint32          aContentType,
                               nsIURI           *aContentLocation,
                               nsIURI           *aRequestingLocation,
                               nsISupports      *aRequestingContext,
                               const nsACString &aMimeGuess,
                               nsISupports      *aExtra,
                               PRInt16          *aDecision)
{
  nsresult rv = NS_OK;
  *aDecision = nsIContentPolicy::ACCEPT;

  NS_ENSURE_ARG_POINTER(aContentLocation);

#ifndef MOZ_THUNDERBIRD
  // Go find out if we are dealing with mailnews. Anything else
  // isn't our concern and we accept content.
  nsCOMPtr<nsIDocShell> docshell;
  rv = GetRootDocShellForContext(aRequestingContext, getter_AddRefs(docshell));
  NS_ENSURE_SUCCESS(rv, rv);

  PRUint32 appType;
  rv = docshell->GetAppType(&appType);
  // We only want to deal with mailnews
  if (NS_FAILED(rv) || appType != nsIDocShell::APP_TYPE_MAIL)
    return NS_OK;
#endif

#ifdef DEBUG_MsgContentPolicy
  nsCString spec;
#endif
  switch(aContentType) {

  case nsIContentPolicy::TYPE_OBJECT:
    // only allow the plugin to load if the allow plugins pref has been set
    if (!mAllowPlugins)
      *aDecision = nsIContentPolicy::REJECT_TYPE;
    return NS_OK;

  case nsIContentPolicy::TYPE_DOCUMENT:
    // At this point, we have no intention of supporting a different JS
    // setting on a subdocument, so we don't worry about TYPE_SUBDOCUMENT here.
   
#ifdef DEBUG_MsgContentPolicy
    (void)aContentLocation->GetSpec(spec);
    fprintf(stderr, "aContentLocation = %s\n", spec.get());
#endif
    
    // If the timing were right, we'd enable JavaScript on the docshell
    // for non mailnews URIs here.  However, at this point, the
    // old document may still be around, so we can't do any enabling just yet.  
    // Instead, we apply the policy in nsIWebProgressListener::OnLocationChange. 
    // For now, we explicitly disable JavaScript in order to be safe rather than
    // sorry, because OnLocationChange isn't guaranteed to necessarily be called
    // soon enough to disable it in time (though bz says it _should_ be called 
    // soon enough "in all sane cases").
    rv = DisableJSOnMailNewsUrlDocshells(aContentLocation, aRequestingContext);

    // if something went wrong during the tweaking, reject this content
    if (NS_FAILED(rv)) {
      *aDecision = nsIContentPolicy::REJECT_TYPE;
      return NS_OK;
    }
    break;

  default:
    break;
  }
  
  // NOTE: Not using NS_ENSURE_ARG_POINTER because this is a legitimate case
  // that can happen.  Also keep in mind that the default policy used for a
  // failure code is ACCEPT.
  if (!aRequestingLocation)
    return NS_ERROR_INVALID_POINTER;

  // if aRequestingLocation is chrome, resource about or file, allow
  // aContentLocation to load
  PRBool isChrome;
  PRBool isRes;
  PRBool isAbout;
  PRBool isFile;

  rv = aRequestingLocation->SchemeIs("chrome", &isChrome);
  rv |= aRequestingLocation->SchemeIs("resource", &isRes);
  rv |= aRequestingLocation->SchemeIs("about", &isAbout);
  rv |= aRequestingLocation->SchemeIs("file", &isFile);

  if (NS_SUCCEEDED(rv) && (isChrome || isRes || isAbout || isFile))
    return rv;

  // Now default to reject so early returns via NS_ENSURE_SUCCESS 
  // cause content to be rejected.
  *aDecision = nsIContentPolicy::REJECT_REQUEST;
  // From here on out, be very careful about returning error codes.
  // An error code will cause the content policy manager to ignore our
  // decision. In most cases, if we get an error code, it's something
  // we didn't expect which means we should be rejecting the request anyway...

  // if aContentLocation is a protocol we handle (imap, pop3, mailbox, etc)
  // or is a chrome url, then allow the load
  nsCAutoString contentScheme;
  PRBool isExposedProtocol = PR_FALSE;
  rv = aContentLocation->GetScheme(contentScheme);
  NS_ENSURE_SUCCESS(rv, NS_OK);

#ifdef MOZ_THUNDERBIRD
  nsCOMPtr<nsIExternalProtocolService> extProtService = do_GetService(NS_EXTERNALPROTOCOLSERVICE_CONTRACTID);
  rv = extProtService->IsExposedProtocol(contentScheme.get(), &isExposedProtocol);
  NS_ENSURE_SUCCESS(rv, NS_OK);
#else
  if (contentScheme.LowerCaseEqualsLiteral("mailto") ||
      contentScheme.LowerCaseEqualsLiteral("news") ||
      contentScheme.LowerCaseEqualsLiteral("snews") ||
      contentScheme.LowerCaseEqualsLiteral("nntp") ||
      contentScheme.LowerCaseEqualsLiteral("imap") ||
      contentScheme.LowerCaseEqualsLiteral("addbook") ||
      contentScheme.LowerCaseEqualsLiteral("pop") ||
      contentScheme.LowerCaseEqualsLiteral("mailbox") ||
      contentScheme.LowerCaseEqualsLiteral("about"))
    isExposedProtocol = PR_TRUE;
#endif

  PRBool isData;

  rv = aContentLocation->SchemeIs("chrome", &isChrome);
  rv |= aContentLocation->SchemeIs("resource", &isRes);
  rv |= aContentLocation->SchemeIs("data", &isData);

  if (isExposedProtocol || (NS_SUCCEEDED(rv) && (isChrome || isRes || isData)))
  {
    *aDecision = nsIContentPolicy::ACCEPT;
    return NS_OK;
  }

  // never load unexposed protocols except for http, https and file. 
  // Protocols like ftp, gopher are always blocked.
  PRBool isHttp;
  PRBool isHttps;
  rv = aContentLocation->SchemeIs("http", &isHttp);
  rv |= aContentLocation->SchemeIs("https", &isHttps);
  rv |= aContentLocation->SchemeIs("file", &isFile);
  if (NS_FAILED(rv) || (!isHttp && !isHttps && !isFile))
    return NS_OK;

  // If we are allowing all remote content...
  if (!mBlockRemoteImages)
  {
    *aDecision = nsIContentPolicy::ACCEPT;
    return NS_OK;
  }

  // Extract the windowtype to handle compose windows separately from mail
  nsCOMPtr<nsIDocShell> rootDocShell;
  rv = GetRootDocShellForContext(aRequestingContext, getter_AddRefs(rootDocShell));
  NS_ENSURE_SUCCESS(rv, NS_OK);
   
  // get the dom document element
  nsCOMPtr<nsIDOMDocument> domDocument = do_GetInterface(rootDocShell, &rv);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsCOMPtr<nsIDOMElement> windowEl;
  rv = domDocument->GetDocumentElement(getter_AddRefs(windowEl));
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString windowType;
  // GetDocumentElement may succeed but return nsnull, if it does, we'll
  // treat the window as a non-msgcompose window.
  if (windowEl)
  {
    rv = windowEl->GetAttribute(NS_LITERAL_STRING("windowtype"), windowType);
    NS_ENSURE_SUCCESS(rv, NS_OK);
  }

  if (windowType.Equals(NS_LITERAL_STRING("msgcompose")))
    ComposeShouldLoad(rootDocShell, aRequestingContext, aContentLocation, aDecision);
  else
  {
    // the remote image could be nested in any number of iframes. For those cases, we don't really
    // care about the value of aRequestingLocation. We care about the parent message pane nsIURI so
    // use that if we can find it. For the non nesting case, mailRequestingLocation and aRequestingLocation
    // should end up being the same object.
    nsCOMPtr<nsIURI> mailRequestingLocation;
    GetMessagePaneURI(rootDocShell, getter_AddRefs(mailRequestingLocation));
    
    MailShouldLoad(mailRequestingLocation ? mailRequestingLocation.get() : aRequestingLocation, 
                   aContentLocation, aDecision);
  }

  return NS_OK;
}

/**
 * For a given msg hdr, iterate through the list of remote content blocking criteria.
 * returns nsIContentPolicy::REJECT if the msg hdr fails any of these tests.
 *
 * @param aRequestingLocation cannot be null
 */
nsresult nsMsgContentPolicy::AllowRemoteContentForMsgHdr(nsIMsgDBHdr * aMsgHdr, nsIURI * aRequestingLocation, nsIURI * aContentLocation, PRInt16 *aDecision)
{
  NS_ENSURE_ARG_POINTER(aMsgHdr);

  // Case #1, check the db hdr for the remote content policy on this particular message
  PRUint32 remoteContentPolicy = kNoRemoteContentPolicy;
  aMsgHdr->GetUint32Property("remoteContentPolicy", &remoteContentPolicy);

  // Case #2, check if the message is in an RSS folder
  PRBool isRSS = PR_FALSE;
  IsRSSArticle(aRequestingLocation, &isRSS);

  // Case #3, the domain for the remote image is in our white list
  PRBool trustedDomain = IsTrustedDomain(aContentLocation);

  // Case 4 is expensive as we're looking up items in the address book. So if
  // either of the two previous items means we load the data, just do it.
  if (isRSS || remoteContentPolicy == kAllowRemoteContent || trustedDomain)
  {
    *aDecision = nsIContentPolicy::ACCEPT;
    return NS_OK;
  }

  // Case #4, author is in our white list..
  PRBool allowForSender = PR_FALSE;
  AllowRemoteContentForSender(aMsgHdr, &allowForSender);

  *aDecision = allowForSender ?
               nsIContentPolicy::ACCEPT : nsIContentPolicy::REJECT_REQUEST;
  
  if (*aDecision == nsIContentPolicy::REJECT_REQUEST && !remoteContentPolicy) // kNoRemoteContentPolicy means we have never set a value on the message
    aMsgHdr->SetUint32Property("remoteContentPolicy", kBlockRemoteContent);
  
  return NS_OK; // always return success
}

/** 
 * Content policy logic for mail windows
 * 
 */
nsresult nsMsgContentPolicy::MailShouldLoad(nsIURI * aRequestingLocation, nsIURI * aContentLocation, PRInt16 * aDecision)
{
  NS_ENSURE_TRUE(aRequestingLocation, NS_OK);

  // Allow remote content when using a remote start page in the message pane.
  // aRequestingLocation is the url currently loaded in the message pane. 
  // If that's an http / https url (as opposed to a mail url) then we 
  // must be loading a start page and not a message.
  PRBool isHttp;
  PRBool isHttps;
  nsresult rv = aRequestingLocation->SchemeIs("http", &isHttp);
  rv |= aRequestingLocation->SchemeIs("https", &isHttps);
  if (NS_SUCCEEDED(rv) && (isHttp || isHttps))
  {
    *aDecision = nsIContentPolicy::ACCEPT;
    return NS_OK;
  }


  // (1) examine the msg hdr value for the remote content policy on this particular message to
  //     see if this particular message has special rights to bypass the remote content check
  // (2) special case RSS urls, always allow them to load remote images since the user explicitly
  //     subscribed to the feed.
  // (3) Check the personal address book and use it as a white list for senders
  //     who are allowed to send us remote images

  // get the msg hdr for the message URI we are actually loading
  nsCOMPtr<nsIMsgMessageUrl> msgUrl = do_QueryInterface(aRequestingLocation, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCString resourceURI;
  msgUrl->GetUri(getter_Copies(resourceURI));

  // get the msg service for this URI
  nsCOMPtr<nsIMsgMessageService> msgService;
  rv = GetMessageServiceFromURI(resourceURI, getter_AddRefs(msgService));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgDBHdr> msgHdr;
  rv = msgService->MessageURIToMsgHdr(resourceURI.get(), getter_AddRefs(msgHdr));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgMailNewsUrl> mailnewsUrl = do_QueryInterface(aRequestingLocation, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  AllowRemoteContentForMsgHdr(msgHdr, aRequestingLocation, aContentLocation, aDecision);

  if (*aDecision == nsIContentPolicy::REJECT_REQUEST)
  {
    // now we need to call out the msg sink informing it that this message has remote content
    nsCOMPtr<nsIMsgWindow> msgWindow;
    rv = mailnewsUrl->GetMsgWindow(getter_AddRefs(msgWindow)); 
    if (msgWindow)
    {
      nsCOMPtr<nsIMsgHeaderSink> msgHdrSink;
      rv = msgWindow->GetMsgHeaderSink(getter_AddRefs(msgHdrSink));
      if (msgHdrSink)
        msgHdrSink->OnMsgHasRemoteContent(msgHdr); // notify the UI to show the remote content hdr bar so the user can overide
    }
  }

  return NS_OK;
}

/** 
 * Content policy logic for compose windows
 * 
 */
nsresult nsMsgContentPolicy::ComposeShouldLoad(nsIDocShell * aRootDocShell, nsISupports * aRequestingContext,
                                               nsIURI * aContentLocation, PRInt16 * aDecision)
{
  nsresult rv;

  nsCOMPtr<nsIDOMWindowInternal> window(do_GetInterface(aRootDocShell, &rv));
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsCOMPtr<nsIMsgComposeService> composeService (do_GetService(NS_MSGCOMPOSESERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsCOMPtr<nsIMsgCompose> msgCompose;
  rv = composeService->GetMsgComposeForWindow(window, getter_AddRefs(msgCompose));
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsCString originalMsgURI;
  msgCompose->GetOriginalMsgURI(getter_Copies(originalMsgURI));
  NS_ENSURE_SUCCESS(rv, NS_OK);

  MSG_ComposeType composeType;
  rv = msgCompose->GetType(&composeType);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  // Only allow remote content for new mail compositions.
  // Block remote content for all other types (drafts, templates, forwards, replies, etc)
  // unless there is an associated msgHdr which allows the load, or unless the image is being
  // added by the user and not the quoted message content...
  if (composeType == nsIMsgCompType::New)
    *aDecision = nsIContentPolicy::ACCEPT;
  else if (!originalMsgURI.IsEmpty())
  {
    nsCOMPtr<nsIMsgDBHdr> msgHdr;
    rv = GetMsgDBHdrFromURI(originalMsgURI.get(), getter_AddRefs(msgHdr));
    NS_ENSURE_SUCCESS(rv, NS_OK);
    AllowRemoteContentForMsgHdr(msgHdr, nsnull, aContentLocation, aDecision);

    // Special case image elements. When replying to a message, we want to allow the 
    // user to add remote images to the message. But we don't want remote images
    // that are a part of the quoted content to load. Fortunately, after the quoted message
    // has been inserted into the document, mail compose flags remote content elements that came 
    // from the original message with a moz-do-not-send attribute. 
    if (*aDecision == nsIContentPolicy::REJECT_REQUEST)
    {
      PRBool insertingQuotedContent = PR_TRUE;
      msgCompose->GetInsertingQuotedContent(&insertingQuotedContent);
      nsCOMPtr<nsIDOMHTMLImageElement> imageElement = do_QueryInterface(aRequestingContext);
      if (!insertingQuotedContent && imageElement)
      {
        PRBool doNotSendAttrib;
        if (NS_SUCCEEDED(imageElement->HasAttribute(NS_LITERAL_STRING("moz-do-not-send"), &doNotSendAttrib)) && 
            !doNotSendAttrib)
           *aDecision = nsIContentPolicy::ACCEPT;
      }
    }
  }

  return NS_OK;
}

nsresult nsMsgContentPolicy::DisableJSOnMailNewsUrlDocshells(
  nsIURI *aContentLocation, nsISupports *aRequestingContext)
{
  // XXX if this class changes so that this method can be called from
  // ShouldProcess, and if it's possible for this to be null when called from
  // ShouldLoad, but not in the corresponding ShouldProcess call,
  // we need to re-think the assumptions underlying this code.
  
  // If there's no docshell to get to, there's nowhere for the JavaScript to 
  // run, so we're already safe and don't need to disable anything.
  if (!aRequestingContext) {
    return NS_OK;
  }

  // the policy we're trying to enforce is around the settings for 
  // message URLs, so if this isn't one of those, bail out
  nsresult rv;
  nsCOMPtr<nsIMsgMessageUrl> msgUrl = do_QueryInterface(aContentLocation, &rv);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

  // since NS_CP_GetDocShellFromContext returns the containing docshell rather
  // than the contained one we need, we can't use that here, so...
  
  nsCOMPtr<nsIFrameLoaderOwner> flOwner = do_QueryInterface(aRequestingContext,
                                                            &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIFrameLoader> frameLoader;
  rv = flOwner->GetFrameLoader(getter_AddRefs(frameLoader));
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(frameLoader, NS_ERROR_INVALID_POINTER);
  
  nsCOMPtr<nsIDocShell> shell;
  rv = frameLoader->GetDocShell(getter_AddRefs(shell));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDocShellTreeItem> docshellTreeItem(do_QueryInterface(shell, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  // what sort of docshell is this?
  PRInt32 itemType;
  rv = docshellTreeItem->GetItemType(&itemType);
  NS_ENSURE_SUCCESS(rv, rv);

  // we're only worried about policy settings in content docshells
  if (itemType != nsIDocShellTreeItem::typeContent) {
    return NS_OK;
  }

  return shell->SetAllowJavascript(PR_FALSE);
}

/**
 * helper routine to get the root docshell for the window requesting the load
 */
nsresult nsMsgContentPolicy::GetRootDocShellForContext(nsISupports * aRequestingContext, nsIDocShell ** aDocShell)
{
  NS_ENSURE_ARG_POINTER(aRequestingContext);
  nsresult rv;

  nsIDocShell *shell = NS_CP_GetDocShellFromContext(aRequestingContext);
  nsCOMPtr<nsIDocShellTreeItem> docshellTreeItem(do_QueryInterface(shell, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDocShellTreeItem> rootItem;
  // we want the app docshell, so don't use GetSameTypeRootTreeItem
  rv = docshellTreeItem->GetRootTreeItem(getter_AddRefs(rootItem));
  NS_ENSURE_SUCCESS(rv, rv);

  return rootItem->QueryInterface(NS_GET_IID(nsIDocShell), (void**) aDocShell);
}

/**
 * helper routine to get the current URI loaded in the message pane for the mail window.
 *
 * @param aRootDocShell the root docshell for a mail window with a message pane (i.e. not a compose window)
 *
 * @return aURI may be null
 */
nsresult nsMsgContentPolicy::GetMessagePaneURI(nsIDocShell * aRootDocShell, nsIURI ** aURI)
{
  nsresult rv;
  nsCOMPtr<nsIDocShellTreeNode> rootDocShellAsNode(do_QueryInterface(aRootDocShell, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDocShellTreeItem> childAsItem;
  rv = rootDocShellAsNode->FindChildWithName(NS_LITERAL_STRING("messagepane").get(),
                                               PR_TRUE, PR_FALSE, nsnull, nsnull, getter_AddRefs(childAsItem));
  NS_ENSURE_SUCCESS(rv, rv);
  
  nsCOMPtr<nsIWebNavigation> webNavigation (do_QueryInterface(childAsItem, &rv));
  NS_ENSURE_SUCCESS(rv, rv);
  
  return webNavigation->GetCurrentURI(aURI);
}

NS_IMETHODIMP
nsMsgContentPolicy::ShouldProcess(PRUint32          aContentType,
                                  nsIURI           *aContentLocation,
                                  nsIURI           *aRequestingLocation,
                                  nsISupports      *aRequestingContext,
                                  const nsACString &aMimeGuess,
                                  nsISupports      *aExtra,
                                  PRInt16          *aDecision)
{
  // XXX Returning ACCEPT is presumably only a reasonable thing to do if we
  // think that ShouldLoad is going to catch all possible cases (i.e. that
  // everything we use to make decisions is going to be available at 
  // ShouldLoad time, and not only become available in time for ShouldProcess).
  // Do we think that's actually the case?
  *aDecision = nsIContentPolicy::ACCEPT;
  return NS_OK;
}

NS_IMETHODIMP nsMsgContentPolicy::Observe(nsISupports *aSubject, const char *aTopic, const PRUnichar *aData)
{
  if (!strcmp(NS_PREFBRANCH_PREFCHANGE_TOPIC_ID, aTopic)) 
  {
    NS_LossyConvertUTF16toASCII pref(aData);

    nsresult rv;

    nsCOMPtr<nsIPrefBranch2> prefBranchInt = do_QueryInterface(aSubject, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    if (pref.Equals(kBlockRemoteImages))
      prefBranchInt->GetBoolPref(kBlockRemoteImages, &mBlockRemoteImages);
  }

  return NS_OK;
}

/** 
 * We implement the nsIWebProgressListener interface in order to enforce
 * settings at onLocationChange time.
 */
NS_IMETHODIMP 
nsMsgContentPolicy::OnStateChange(nsIWebProgress *aWebProgress,
                                  nsIRequest *aRequest, PRUint32 aStateFlags,
                                  nsresult aStatus)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgContentPolicy::OnProgressChange(nsIWebProgress *aWebProgress,
                                     nsIRequest *aRequest,
                                     PRInt32 aCurSelfProgress,
                                     PRInt32 aMaxSelfProgress,
                                     PRInt32 aCurTotalProgress,
                                     PRInt32 aMaxTotalProgress)
{
  return NS_OK;
}

NS_IMETHODIMP 
nsMsgContentPolicy::OnLocationChange(nsIWebProgress *aWebProgress,
                                     nsIRequest *aRequest, nsIURI *aLocation)
{
  nsresult rv;

  // If anything goes wrong and/or there's no docshell associated with this
  // request, just give up.  The behavior ends up being "don't consider 
  // re-enabling JS on the docshell", which is the safe thing to do (and if
  // the problem was that there's no docshell, that means that there was 
  // nowhere for any JavaScript to run, so we're already safe
  
  nsCOMPtr<nsIDocShell> docShell = do_QueryInterface(aWebProgress, &rv);
  if (NS_FAILED(rv)) {
    return NS_OK;
  }

#ifdef DEBUG
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(aRequest, &rv);
  nsCOMPtr<nsIDocShell> docShell2;
  NS_QueryNotificationCallbacks(channel, docShell2);
  NS_ASSERTION(docShell == docShell2, "aWebProgress and channel callbacks"
                                      " do not point to the same docshell");
#endif
  
  // If this is a mailnews url, turn off JavaScript, otherwise turn it on
  nsCOMPtr<nsIMsgMessageUrl> messageUrl = do_QueryInterface(aLocation, &rv);
  return docShell->SetAllowJavascript(NS_FAILED(rv));
}

NS_IMETHODIMP
nsMsgContentPolicy::OnStatusChange(nsIWebProgress *aWebProgress,
                                   nsIRequest *aRequest, nsresult aStatus,
                                   const PRUnichar *aMessage)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgContentPolicy::OnSecurityChange(nsIWebProgress *aWebProgress,
                                     nsIRequest *aRequest, PRUint32 aState)
{
  return NS_OK;
}

#ifdef MOZ_THUNDERBIRD

NS_IMPL_ISUPPORTS1(nsMsgCookiePolicy, nsICookiePermission)


NS_IMETHODIMP nsMsgCookiePolicy::SetAccess(nsIURI         *aURI,
                                           nsCookieAccess  aAccess)
{
  // we don't support cookie access lists for mail
  return NS_OK;
}

NS_IMETHODIMP nsMsgCookiePolicy::CanAccess(nsIURI         *aURI,
                                           nsIChannel     *aChannel,
                                           nsCookieAccess *aResult)
{
  // by default we deny all cookies in mail
  *aResult = ACCESS_DENY;
  NS_ENSURE_ARG_POINTER(aChannel);
  
  nsCOMPtr<nsILoadContext> loadContext;
  NS_QueryNotificationCallbacks(aChannel, loadContext);

  NS_ENSURE_TRUE(loadContext, NS_OK);
  PRBool isContent;
  loadContext->GetIsContent(&isContent);

  // allow chrome to set cookies
  if (!isContent)
    *aResult = ACCESS_DEFAULT;
  else // allow RSS articles in content to access cookies
  {
    NS_ENSURE_TRUE(aURI, NS_OK);  
    PRBool isRSS = PR_FALSE;
    IsRSSArticle(aURI, &isRSS);
    if (isRSS)
      *aResult = ACCESS_DEFAULT;
  }

  return NS_OK;
}

NS_IMETHODIMP nsMsgCookiePolicy::CanSetCookie(nsIURI     *aURI,
                                              nsIChannel *aChannel,
                                              nsICookie2 *aCookie,
                                              PRBool     *aIsSession,
                                              PRInt64    *aExpiry,
                                              PRBool     *aResult)
{
  *aResult = PR_TRUE;
  return NS_OK;
}

NS_IMETHODIMP nsMsgCookiePolicy::GetOriginatingURI(nsIChannel  *aChannel,
                                                   nsIURI     **aURI)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

#endif
