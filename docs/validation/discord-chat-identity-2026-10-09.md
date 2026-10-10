# Discord conversational identity validation — 2026-10-09

All conversational replies now use the authenticated Discord application bot account,
Aimee#5282. This includes human chat, bot conversations, and replies in threads.
Startup announcements and explicit operator `send-stdin` delivery still use the webhook.

Validation:

- 49 bridge and model preparation tests passed. Coverage includes human/channel and
  thread routing, peer recipients, notification suppression, message splitting,
  memory capture ordering, and history commit only after successful delivery.
- Deployed on CT 9211 at 192.168.1.253 using image
  `aimee-discord-bridge:one-sender-20261009`; container confirmed running.
- Deployed `/app/bridge.py` matches the tested source SHA-256:
  `84212de786c0a16545e90b33fb7aba690e28f8f7a5ec9d5c3ca2bd9899d0a797`.
- Read-only authenticated Discord verification confirmed username Aimee,
  discriminator 5282, bot status, and View Channel, Send Messages, and Send Messages
  in Threads permissions in the configured channel.
- Deployed turn processor has one bot-account delivery call and no webhook delivery
  call. No synthetic conversational messages were posted to Discord for validation.
