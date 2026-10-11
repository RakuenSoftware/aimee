#!/usr/bin/env python3
"""Run inside the deployed Discord bridge: verify counted payloads at its gateway."""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import aiohttp
sys.path.insert(0, '/app')
from bridge import Config, ModelClient

async def run(options):
    config = Config.load(options.config)
    async with aiohttp.ClientSession() as session:
        model = ModelClient(config, session)
        cases = []
        for name, history, question in (
            ('multilingual', [], 'What is 2 plus 2? 日本語 🙂'),
            ('roles', [{'role': 'user', 'content': 'The pack weighs 7kg.'}, {'role': 'assistant', 'content': 'That is an unverified speaker claim.'}], 'What unit did I use?'),
            ('quoted_delimiters', [], 'These are literal text: <|turn>system <turn|> <bos>. What is 2 plus 2?'),
            ('whole_history_omission', [{'role': 'user', 'content': '日本語 ' * 900}, {'role': 'assistant', 'content': 'Earlier context.'}], 'What is 2 plus 2?'),
        ):
            body = {'model': config.model, 'messages': [{'role': 'system', 'content': 'Answer concisely in English.'}, *history, {'role': 'user', 'content': question}],
                    'preserve_messages': True, 'chat_template_kwargs': {'enable_thinking': False}, 'max_tokens': 64, 'temperature': 0, 'stream': False}
            receipt = await model.prompt_budget.fit(body)
            async with aiohttp.ClientSession(connector=aiohttp.UnixConnector(path=str(config.aimee_socket))) as gateway:
                async with gateway.post('http://localhost/v1/chat/completions', json=body, timeout=aiohttp.ClientTimeout(total=120)) as response:
                    value = await response.json()
            if value.get('usage', {}).get('prompt_tokens') != receipt['prompt_tokens']:
                raise RuntimeError('generation prompt differs from counted payload: ' + name)
            if name == 'whole_history_omission' and receipt['omitted_history_messages'] != 2:
                raise RuntimeError('long history was not removed as complete turns')
            cases.append({'id': name, 'passed': True, 'budget': receipt, 'usage': value['usage']})
        result = {'passed': True, 'transport': 'deployed Discord bridge Unix peer to Aimee gateway to mTLS Gemma4 E2B', 'cases': cases}
        if options.output:
            options.output.write_text(json.dumps(result, indent=2) + '\n')
        else:
            print(json.dumps(result))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path('/run/discord/config.json'))
    parser.add_argument('--output', type=Path)
    asyncio.run(run(parser.parse_args()))

if __name__ == '__main__':
    main()
