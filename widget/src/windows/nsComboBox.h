/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef nsComboBox_h__
#define nsComboBox_h__

#include "nsdefs.h"
#include "nsWindow.h"
#include "nsSwitchToUIThread.h"
#include "nsIComboBox.h"

/**
 * Native Win32 Combobox wrapper
 */

class nsComboBox : public nsWindow,
                   public nsIListWidget,
                   public nsIComboBox
{

public:
    nsComboBox();
    ~nsComboBox();

        // nsISupports
    NS_IMETHOD QueryInterface(REFNSIID aIID, void** aInstancePtr);                           
    NS_IMETHOD_(nsrefcnt) AddRef(void);                                       
    NS_IMETHOD_(nsrefcnt) Release(void);          

    // nsIWidget overrides
    virtual PRBool OnMove(PRInt32 aX, PRInt32 aY);
    virtual PRBool OnPaint();
    virtual PRBool OnResize(nsRect &aWindowRect);

    // nsIWidget
    NS_IMETHOD     GetBounds(nsRect &aRect);

    // nsIComboBox interface
    NS_IMETHOD      AddItemAt(nsString &aItem, PRInt32 aPosition);
    virtual PRInt32 FindItem(nsString &aItem, PRInt32 aStartPos);
    virtual PRInt32 GetItemCount();
    virtual PRBool  RemoveItemAt(PRInt32 aPosition);
    virtual PRBool  GetItemAt(nsString& anItem, PRInt32 aPosition);
    NS_IMETHOD      GetSelectedItem(nsString& aItem);
    virtual PRInt32 GetSelectedIndex();
    NS_IMETHOD      SelectItem(PRInt32 aPosition);
    NS_IMETHOD      Deselect() ;

    NS_IMETHOD      PreCreateWidget(nsWidgetInitData *aInitData);

protected:

      // Modify the height passed to create and resize to be 
      // the combo box drop down list height.
    PRInt32 GetHeight(PRInt32 aProposedHeight);    
    LPCTSTR WindowClass();
    DWORD   WindowStyle();
    DWORD   WindowExStyle();
    PRInt32 mDropDownHeight;

};

#endif // nsComboBox_h__
