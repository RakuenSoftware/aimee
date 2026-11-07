package ir

import (
 "crypto/sha256"
 "encoding/hex"
 "encoding/json"
 "strings"
 "unicode/utf8"
)

// InputAttachment is opaque host provenance, never model input or use authority.
type InputAttachment struct {
 SchemaVersion int `json:"schema_version"`
 Kind string `json:"kind"`
 OperationUUID string `json:"operation_uuid"`
 TextSHA256 string `json:"text_sha256"`
 Opaque string `json:"opaque"`
}
func canonicalInputUUID(s string) bool {
 if len(s)!=36 || s=="00000000-0000-0000-0000-000000000000" {return false}
 for i,c:=range s {if i==8||i==13||i==18||i==23 {if c!='-' {return false}} else if !(c>='0'&&c<='9'||c>='a'&&c<='f'){return false}}
 return true
}
func ValidInputAttachment(raw json.RawMessage,text string) bool {
 var fields map[string]json.RawMessage; var a InputAttachment
 if !Decode(raw,&fields)||len(fields)!=5||!Decode(raw,&a)||a.SchemaVersion!=1||a.Kind!="host.original-input.v1"||!canonicalInputUUID(a.OperationUUID)||len(a.TextSHA256)!=64||strings.Trim(a.TextSHA256,"0123456789abcdef")!=""||len(a.Opaque)==0||len(a.Opaque)>262144||!utf8.ValidString(a.Opaque)||strings.ContainsRune(a.Opaque,0) {return false}
 hash:=sha256.Sum256([]byte(text));return a.TextSHA256==hex.EncodeToString(hash[:])
}
func MessageHasInputAttachments(raw json.RawMessage) bool {var m Message;if !Decode(raw,&m){return false};for _,b:=range m.Blocks{if b.InputAttachment!=nil{return true}};return false}
func EventHasInputAttachments(raw json.RawMessage) bool {var e struct{Kind string `json:"kind"`;Payload json.RawMessage `json:"payload"`};if json.Unmarshal(raw,&e)!=nil{return false};if e.Kind=="message"{return MessageHasInputAttachments(e.Payload)};if e.Kind=="history"{var h HistoryEvent;if Decode(e.Payload,&h){return MessageHasInputAttachments(h.Message)}};return false}
// Keep every admitted original-input pair exactly once, in chronology. The
// checkpoint cannot mint/drop/replace opaque input provenance. Other ordinary
// messages may change; the attachment confers no permission to resolve a value.
func InputAttachmentsPreserved(current,next []Message) bool {
 type pair struct{Text string;Attachment InputAttachment};extract:=func(ms []Message)([]pair,bool){var out []pair;seen:=map[string]bool{};for _,m:=range ms{for _,b:=range m.Blocks{if b.InputAttachment==nil{continue};if m.Role!="user"||b.Type!="text"||b.Text==nil||seen[b.InputAttachment.OperationUUID]{return nil,false};raw,_:=json.Marshal(b.InputAttachment);if !ValidInputAttachment(raw,*b.Text){return nil,false};seen[b.InputAttachment.OperationUUID]=true;out=append(out,pair{*b.Text,*b.InputAttachment})}};return out,true}
 a,ok:=extract(current);if !ok{return false};b,ok:=extract(next);if !ok||len(a)!=len(b){return false};for i:=range a{if a[i]!=b[i]{return false}};return true
}

// ValidInputOperationUUID shares the original-input wire identity rules.
func ValidInputOperationUUID(value string) bool { return canonicalInputUUID(value) }
