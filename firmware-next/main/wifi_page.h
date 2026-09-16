#pragma once

static const char WIFI_MANAGER_PAGE[] = R"HTML(
<section id="saved-wifi"><h2>已保存 Wi-Fi</h2><p>裝置會保存最多 4 組網路。連線正常時不會只因其他網路訊號較強而切換；斷線後才會重新掃描並自動改連可用網路。</p><p id="wifi-count">正在讀取…</p><div id="wifi-profiles" class="wifi-profiles" aria-live="polite"></div><h3>新增或更新 Wi-Fi</h3><form id="wifi-form"><input type="hidden" name="nonce" value="WIFI_NONCE"><label>掃描到的網路<select id="wifi-networks"><option value="">手動輸入網路名稱</option></select></label><button id="wifi-rescan" type="button">重新掃描附近網路</button><label>網路名稱<input id="wifi-ssid" name="ssid" required maxlength="32" autocomplete="off"></label><label>密碼<input name="password" type="password" minlength="8" maxlength="63" autocomplete="new-password"><small>開放網路請留空；其他密碼須為 8 至 63 bytes。</small></label><button id="wifi-save">連線成功後保存</button></form><p id="wifi-status" role="status" aria-live="polite"></p><p class="help">加入其他網路後，裝置日後可能改連該網路並取得新的 IP。請從裝置的「手機設定」重新掃描網址。</p></section>
<style>#wifi-rescan{min-height:44px;width:100%;margin-top:8px}.wifi-profiles{display:grid;gap:10px}.wifi-profile{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px 12px;align-items:center;border:1px solid #d9e2dc;padding:12px;border-radius:12px;overflow-wrap:anywhere}.wifi-name{min-width:0;font-weight:700}.wifi-meta{font-size:14px;color:#63716c}.wifi-actions{grid-column:2;grid-row:1/3;display:flex;gap:8px;flex-wrap:wrap}.wifi-actions button{min-height:44px;margin:0}.wifi-current{display:inline-block;background:#dcefe5;color:#285843;border-radius:999px;padding:2px 8px;margin-left:8px;font-size:12px}.danger{background:#a33b34}.danger.secondary{background:#f4e7e5;color:#84332d}small{display:block;color:#63716c;font-weight:400}@media(max-width:767px){.wifi-profile{grid-template-columns:1fr}.wifi-actions{grid-column:1;grid-row:auto}.wifi-actions button{flex:1 1 130px;width:100%}}@media(min-width:768px) and (max-width:1023px){.wifi-profiles{grid-template-columns:repeat(2,minmax(0,1fr))}}</style>
<script>
(()=>{const nonce='WIFI_NONCE',profilesEl=document.querySelector('#wifi-profiles'),countEl=document.querySelector('#wifi-count'),statusEl=document.querySelector('#wifi-status'),form=document.querySelector('#wifi-form'),ssidEl=document.querySelector('#wifi-ssid'),networksEl=document.querySelector('#wifi-networks');let removePending='',mutationPending=false;
function message(text){statusEl.textContent=text}
function button(text,className){const b=document.createElement('button');b.type='button';b.textContent=text;if(className)b.className=className;return b}
function render(d){countEl.textContent='已保存 '+d.profiles.length+' / 4 組';profilesEl.replaceChildren();for(const profile of d.profiles){const row=document.createElement('div');row.className='wifi-profile';const name=document.createElement('div');name.className='wifi-name';name.textContent=profile.ssid;if(profile.connected){const badge=document.createElement('span');badge.className='wifi-current';badge.textContent='目前連線';name.append(badge)}const meta=document.createElement('div');meta.className='wifi-meta';meta.textContent=profile.visible&&profile.rssi!==null?'訊號 '+profile.rssi+' dBm':'目前未掃描到';const actions=document.createElement('div');actions.className='wifi-actions';const remove=button(removePending===profile.ssid?'再按一次確認移除':'移除',removePending===profile.ssid?'danger':'danger secondary');remove.onclick=()=>{if(mutationPending)return;if(removePending!==profile.ssid){removePending=profile.ssid;render(d);message('再次按下可移除「'+profile.ssid+'」。移除會中斷目前連線，再從更新後的清單重新選擇網路。');return}mutationPending=true;remove.disabled=true;fetch('/api/wifi/remove',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Setup-Nonce':nonce},body:new URLSearchParams({ssid:profile.ssid})}).then(async r=>{const text=await r.text();if(!r.ok)throw Error(text);removePending='';message(text);await refresh()}).catch(e=>message(e.message||'移除失敗，請重新整理狀態後再試。')).finally(()=>mutationPending=false);actions.append(remove);row.append(name,meta,actions);profilesEl.append(row)}}}
async function refresh(){try{const r=await fetch('/api/wifi',{cache:'no-store'});if(!r.ok)throw Error();const d=await r.json();render(d);networksEl.replaceChildren(new Option('手動輸入網路名稱',''));for(const network of d.networks){networksEl.append(new Option(network.ssid+' ('+network.rssi+' dBm)',network.ssid))}if(d.storageFault)message('Wi-Fi 儲存結果待確認，暫停修改；裝置正在重新載入確認。');else if(d.connecting)message('正在嘗試新網路，失敗時會繼續使用已保存網路。');else if(d.failed)message('新網路連線失敗；已恢復自動連線，配網熱點仍可使用。');else if(d.connected)message('目前已連線。')}catch(e){message('無法取得 Wi-Fi 狀態，請確認仍連上裝置。')}}
const rescanEl=document.querySelector('#wifi-rescan');let scanPoll=0;
rescanEl.onclick=async()=>{
  if(mutationPending)return;
  mutationPending=true;rescanEl.disabled=true;
  try{
    const r=await fetch('/api/wifi/scan',{method:'POST',headers:{'X-Setup-Nonce':nonce}});
    const text=await r.text();if(!r.ok)throw Error(text);
    message('正在掃描無線網路，請稍候…');clearInterval(scanPoll);let tries=0;
    scanPoll=setInterval(async()=>{
      tries+=1;await refresh();const found=networksEl.options.length-1;
      if(found>0){clearInterval(scanPoll);message('掃描完成，共 '+found+' 個網路。');}
      else if(tries>=12){clearInterval(scanPoll);message('沒有掃描到無線網路；裝置只支援 2.4 GHz，請確認熱點是 2.4 GHz。');}
    },1200);
  }catch(e){message(e.message||'無法開始掃描，請稍後再試。');}
  finally{mutationPending=false;rescanEl.disabled=false;}
};
networksEl.onchange=()=>{if(networksEl.value)ssidEl.value=networksEl.value};
form.onsubmit=async e=>{
  e.preventDefault();if(mutationPending)return;mutationPending=true;message('正在連線；成功後才會保存。');
  try{
    const r=await fetch('/setup',{method:'POST',body:new URLSearchParams(new FormData(form))});
    const text=await r.text();if(!r.ok)throw Error(text);
    form.querySelector('input[type=password]').value='';await refresh();
  }catch(e){message(e.message||'送出失敗，請確認仍連上裝置。');}
  finally{mutationPending=false;}
};
refresh();setInterval(refresh,3000);
})();
</script>
)HTML";
