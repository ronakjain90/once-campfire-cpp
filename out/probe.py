import sys, re, json
sys.path.insert(0,'/Volumes/ExternalHD/Code/AI/once-campfire/wt/T13/tools/diffsweep/lib')
import harness
from httpx import Client, multipart, urlencode
L=harness.seed_labels('default')
a=harness.App('expected',sys.argv[1] if len(sys.argv)>1 else 'campfire-rust:app',4501,'probe'); a.start('default')
def show(r,n=700):
    print(r.status, [h for h in r.headers if h[0] in('location','content-type')]); print(r.text()[:n]); print('---')
try:
    c=Client(4501,'127.0.0.1:4390')
    r=c.request('POST','/session',{'Content-Type':'application/x-www-form-urlencoded','Sec-Fetch-Site':'same-origin'},urlencode({'email_address':L['emails.david'],'password':L['passwords.all']})); print(r.status)
    des=L['rooms.designers']
    body,ct=multipart({'message[body]':'<p>Hi</p>','message[client_message_id]':'cm1'},[])
    h={'Accept':'text/vnd.turbo-stream.html','Content-Type':ct,'Sec-Fetch-Site':'same-origin'}
    show(c.request('POST',f'/rooms/{des}/messages',h,body),1500)
    h2={'Accept':'text/vnd.turbo-stream.html','Content-Type':'application/x-www-form-urlencoded','Sec-Fetch-Site':'same-origin'}
    h3={'Content-Type':'application/x-www-form-urlencoded','Sec-Fetch-Site':'same-origin'}
    show(c.request('POST','/users/me/push_subscriptions',h3,urlencode({'push_subscription[endpoint]':'https://127.0.0.1:9/p','push_subscription[p256dh_key]':'k','push_subscription[auth_key]':'a'})))
    show(c.request('POST','/account/bots',h2,urlencode({'user[name]':'Robo'})))
    t=c.request('GET','/account/bots').text(); i=t.find('Robo'); print(t[i-600:i+100])
    show(c.request('GET',f"/messages/{L['messages.unboosted']}/boosts"),600)
finally: a.stop()
