#!/usr/bin/env python3
"""Turn the captured POST response body (post 3) into a template with \\x01NAME\\x02 slots."""
import re,sys
b=open('fixtures/post_sample.html',encoding='utf-8').read()
def slot(n): return '\x01'+n+'\x02'
def rep(old,new,count=None):
    global b
    n=b.count(old); assert n>0,(old); 
    if count is not None: assert n==count,(old,n)
    b=b.replace(old,new)
rep('src="/users/eyJfcmFpbHMiOnsiZGF0YSI6MTI3MzI2MTQxLCJwdXIiOiJ1c2VyL2F2YXRhciJ9fQ--0ac8233a4786ee6c413408416bb81e7107534db5702d351533dc2ecd3876e97f/avatar?v=20260102160000"','src="'+slot('AVATAR')+'"',1)
rep('data-message-updated-at="1791201842876"','data-message-updated-at="'+slot('UMS')+'"',1)
rep('1791201842876',slot('CMS'),2)
rep('2026-10-05T12:04:02Z',slot('ISO'),2)
rep('message_18dba095eb6bd9fb3','message_'+slot('CID'))
rep('messages_rooms_open_201306877','messages_'+slot('ROOMDOM'),1)
rep('201306877',slot('RID'))
rep('127326141',slot('UID'))
rep('>HQ</a>','>'+slot('RNAME')+'</a>',1)
rep('David',slot('UNAME'))
rep('bench write 3',slot('BODY'),1)
rep('933434641',slot('MID'))
assert '1791201842' not in b
open('fixtures/post_template.txt','w',encoding='utf-8').write(b)
print(sorted(set(re.findall('\x01(\\w+)\x02',b))))
