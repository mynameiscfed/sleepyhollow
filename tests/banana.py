#!/usr/bin/env python
# -*- coding: utf-8 -*-
import re
from lxml import html as lhtml
from sleepyhollow import SleepyHollow


class GetASaleProduct(object):
    meta_redirect_url = re.compile(r'meta\s+'
                                   'http-equiv="refresh"\s+'
                                   'content="\d+;URL=(?P<url>.*?)"', re.I)

    def __init__(self):
        self.http = SleepyHollow()

    def get_response_with_dom(self, url):
        if not url.startswith('http'):
            url = 'http://www.bananarepublic.com/%s' % url.lstrip('/')

        response = self.http.get(url, config=dict(screenshot=True))
        meta_refresh = self.meta_redirect_url.search(response.html)

        if meta_refresh is not None:
            return self.get_response_with_dom(meta_refresh.group('url'))

        response.dom = lhtml.fromstring(response.html)
        return response

    def find_sale_links(self):
        print "Getting sales links..."
        response = self.get_response_with_dom('http://www.bananarepublic.com/products/index.jsp')
        return response.dom.xpath("//ul/li[contains(@class, 'idxBottomCat')]/a["
                                  "contains(text(), 'Sale') or "
                                  "contains(text(), 'Clearance') or "
                                  "contains(text(), 'Discount')]/@href")

    def find_product_links(self, category_link):
        print "Getting product links..."

        response = self.get_response_with_dom(category_link)

        return response.dom.xpath("//a[contains(@class, 'productItemName')]/@href")

    def start(self):
        for category_link in self.find_sale_links():
            for product_link in self.find_product_links(category_link):
                response = self.get_response_with_dom(product_link)
                img = response.dom.cssselect("#product_image")[0]
                src = img.attrib['src']
                assert src.lower().endswith('jpg'), 'Expected %r to be a JPG' % src
                break
            break
GetASaleProduct().start()
