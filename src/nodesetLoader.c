/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2019 (c) Matthias Konnerth
 */

#define _POSIX_C_SOURCE 199309L
#include "nodesetLoader.h"
#include "nodeset.h"
#include "util.h"
#include <libxml/SAX.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    PARSER_STATE_INIT,
    PARSER_STATE_NODE,
    PARSER_STATE_DISPLAYNAME,
    PARSER_STATE_REFERENCES,
    PARSER_STATE_REFERENCE,
    PARSER_STATE_DESCRIPTION,
    PARSER_STATE_ALIAS,
    PARSER_STATE_UNKNOWN,
    PARSER_STATE_NAMESPACEURIS,
    PARSER_STATE_URI
} TParserState;

struct TParserCtx {
    TParserState state;
    TParserState prev_state;
    size_t unknown_depth;
    TNodeClass nodeClass;
    TNode *node;
    char **nextOnCharacters;
    void *userContext;
};

static char *getAttributeValue(NodeAttribute *attr, const char **attributes,
                               int nb_attributes) {
    const int fields = 5;
    for(int i = 0; i < nb_attributes; i++) {
        const char *localname = attributes[i * fields + 0];
        if(strcmp((const char *)localname, attr->name))
            continue;
        const char *value_start = attributes[i * fields + 3];
        const char *value_end = attributes[i * fields + 4];
        size_t size = (size_t)(value_end - value_start);
        char *value = (char *)malloc(sizeof(char) * size + 1);
        nodeset->countedChars[nodeset->charsSize++] = value;
        memcpy(value, value_start, size);
        value[size] = '\0';
        return value;
    }
    if(attr->defaultValue != NULL || attr->optional) {
        return attr->defaultValue;
    }
    // todo: remove this assertation

    printf("attribute: %s\n", attr->name);
    assertf(false, "attribute not found, no default value set in getAttributeValue\n");
}

static void extractAttributes(const TNamespace *namespaces, TNode *node,
                              int attributeSize, const char **attributes) {
    node->id = extractNodedId(namespaces,
                              getAttributeValue(&attrNodeId, attributes, attributeSize));
    node->browseName = getAttributeValue(&attrBrowseName, attributes, attributeSize);
    switch(node->nodeClass) {
        case NODECLASS_OBJECTTYPE: {
            ((TObjectTypeNode *)node)->isAbstract =
                getAttributeValue(&attrIsAbstract, attributes, attributeSize);
            break;
        }
        case NODECLASS_OBJECT: {
            ((TObjectNode *)node)->parentNodeId =
                extractNodedId(namespaces, getAttributeValue(&attrParentNodeId,
                                                             attributes, attributeSize));
            ((TObjectNode *)node)->eventNotifier =
                getAttributeValue(&attrEventNotifier, attributes, attributeSize);
            break;
        }
        case NODECLASS_VARIABLE: {

            ((TVariableNode *)node)->parentNodeId =
                extractNodedId(namespaces, getAttributeValue(&attrParentNodeId,
                                                             attributes, attributeSize));
            char *datatype = getAttributeValue(&attrDataType, attributes, attributeSize);
            TNodeId aliasId = alias2Id(datatype);
            if(aliasId.id != 0) {
                ((TVariableNode *)node)->datatype = aliasId;
            } else {
                ((TVariableNode *)node)->datatype = extractNodedId(namespaces, datatype);
            }
            ((TVariableNode *)node)->valueRank =
                getAttributeValue(&attrValueRank, attributes, attributeSize);
            ((TVariableNode *)node)->arrayDimensions =
                getAttributeValue(&attrArrayDimensions, attributes, attributeSize);

            break;
        }
        case NODECLASS_VARIABLETYPE: {

            ((TVariableTypeNode *)node)->valueRank =
                getAttributeValue(&attrValueRank, attributes, attributeSize);
            char *datatype = getAttributeValue(&attrDataType, attributes, attributeSize);
            TNodeId aliasId = alias2Id(datatype);
            if(aliasId.id != 0) {
                ((TVariableTypeNode *)node)->datatype = aliasId;
            } else {
                ((TVariableTypeNode *)node)->datatype =
                    extractNodedId(namespaces, datatype);
            }
            ((TVariableTypeNode *)node)->arrayDimensions =
                getAttributeValue(&attrArrayDimensions, attributes, attributeSize);
            ((TVariableTypeNode *)node)->isAbstract =
                getAttributeValue(&attrIsAbstract, attributes, attributeSize);
            break;
        }
        case NODECLASS_DATATYPE:;
            break;
        case NODECLASS_METHOD:
            ((TMethodNode *)node)->parentNodeId =
                extractNodedId(namespaces, getAttributeValue(&attrParentNodeId,
                                                             attributes, attributeSize));
            break;
        case NODECLASS_REFERENCETYPE:;
            break;
        default:;
    }
}

static void initNode(TNamespace *namespaces, TNodeClass nodeClass, TNode *node,
                     int nb_attributes, const char **attributes) {
    node->nodeClass = nodeClass;
    node->hierachicalRefs = NULL;
    node->nonHierachicalRefs = NULL;
    node->browseName = NULL;
    node->displayName = NULL;
    node->description = NULL;
    node->writeMask = NULL;
    extractAttributes(namespaces, node, nb_attributes, attributes);
}

static void extractReferenceAttributes(TParserCtx *ctx, int attributeSize,
                                       const char **attributes) {
    Reference *newRef = (Reference *)malloc(sizeof(Reference));
    newRef->target.idString = NULL;
    newRef->target.id = NULL;
    newRef->refType.idString = NULL;
    newRef->refType.id = NULL;
    nodeset->countedRefs[nodeset->refsSize++] = newRef;
    newRef->next = NULL;
    if(strEqual("true", getAttributeValue(&attrIsForward, attributes, attributeSize))) {
        newRef->isForward = true;
    } else {
        newRef->isForward = false;
    }
    newRef->refType =
        extractNodedId(nodeset->namespaceTable->ns,
                       getAttributeValue(&attrReferenceType, attributes, attributeSize));
    if(isHierachicalReference(newRef)) {
        Reference **lastRef = &ctx->node->hierachicalRefs;
        while(*lastRef) {
            lastRef = &(*lastRef)->next;
        }
        *lastRef = newRef;
    } else {
        Reference **lastRef = &ctx->node->nonHierachicalRefs;
        while(*lastRef) {
            lastRef = &(*lastRef)->next;
        }
        *lastRef = newRef;
    }
    ctx->nextOnCharacters = &newRef->target.idString;
}

static void enterUnknownState(TParserCtx *ctx) {
    ctx->prev_state = ctx->state;
    ctx->state = PARSER_STATE_UNKNOWN;
    ctx->unknown_depth = 1;
}

static void OnStartElementNs(void *ctx, const char *localname, const char *prefix,
                             const char *URI, int nb_namespaces, const char **namespaces,
                             int nb_attributes, int nb_defaulted,
                             const char **attributes) {

    TParserCtx *pctx = (TParserCtx *)ctx;
    switch(pctx->state) {
        case PARSER_STATE_INIT:
            if(strEqual(localname, ALIAS)) {
                pctx->state = PARSER_STATE_ALIAS;
                pctx->node = NULL;
                nodeset->aliasArray[nodeset->aliasSize] = (Alias *)malloc(sizeof(Alias));
                nodeset->aliasArray[nodeset->aliasSize]->id.idString = NULL;
                nodeset->aliasArray[nodeset->aliasSize]->name =
                    getAttributeValue(&attrAlias, attributes, nb_attributes);
                pctx->nextOnCharacters =
                    &nodeset->aliasArray[nodeset->aliasSize]->id.idString;
                pctx->state = PARSER_STATE_ALIAS;
            } else if(strEqual(localname, OBJECT)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_OBJECT;
                pctx->node = (TNode *)malloc(sizeof(TObjectNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, OBJECTTYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_OBJECTTYPE;
                pctx->node = (TNode *)malloc(sizeof(TObjectTypeNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, VARIABLE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_VARIABLE;
                pctx->node = (TNode *)malloc(sizeof(TVariableNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, DATATYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_DATATYPE;
                pctx->node = (TNode *)malloc(sizeof(TDataTypeNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, METHOD)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_METHOD;
                pctx->node = (TNode *)malloc(sizeof(TMethodNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, REFERENCETYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_REFERENCETYPE;
                pctx->node = (TNode *)malloc(sizeof(TReferenceTypeNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, VARIABLETYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_VARIABLETYPE;
                pctx->node = (TNode *)malloc(sizeof(TVariableTypeNode));
                initNode(nodeset->namespaceTable->ns, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, NAMESPACEURIS)) {
                pctx->state = PARSER_STATE_NAMESPACEURIS;
            } else if(strEqual(localname, "UANodeSet") ||
                      strEqual(localname, "Aliases") ||
                      strEqual(localname, "Extensions")) {
                pctx->state = PARSER_STATE_INIT;
            } else {
                enterUnknownState(pctx);
            }
            break;
        case PARSER_STATE_NAMESPACEURIS:
            if(strEqual(localname, NAMESPACEURI)) {
                nodeset->namespaceTable->size++;
                TNamespace *ns = (TNamespace *)realloc(
                    nodeset->namespaceTable->ns,
                    sizeof(TNamespace) * (nodeset->namespaceTable->size));
                nodeset->namespaceTable->ns = ns;
                ns[nodeset->namespaceTable->size - 1].name = NULL;
                pctx->nextOnCharacters = &ns[nodeset->namespaceTable->size - 1].name;
                pctx->state = PARSER_STATE_URI;
            } else {
                enterUnknownState(pctx);
            }
            break;
        case PARSER_STATE_URI:
            enterUnknownState(pctx);
            break;
        case PARSER_STATE_NODE:
            if(strEqual(localname, DISPLAYNAME)) {
                pctx->nextOnCharacters = &pctx->node->displayName;
                pctx->state = PARSER_STATE_DISPLAYNAME;
            } else if(strEqual(localname, REFERENCES)) {
                pctx->state = PARSER_STATE_REFERENCES;
            } else if(strEqual(localname, DESCRIPTION)) {
                pctx->state = PARSER_STATE_DESCRIPTION;
            } else {
                enterUnknownState(pctx);
            }
            break;

        case PARSER_STATE_REFERENCES:
            if(strEqual(localname, REFERENCE)) {
                pctx->state = PARSER_STATE_REFERENCE;
                extractReferenceAttributes(pctx, nb_attributes, attributes);
            } else {
                enterUnknownState(pctx);
            }
            break;
        case PARSER_STATE_DESCRIPTION:
            enterUnknownState(pctx);
            break;
        case PARSER_STATE_ALIAS:
            enterUnknownState(pctx);
            break;
        case PARSER_STATE_DISPLAYNAME:
            enterUnknownState(pctx);
            break;
        case PARSER_STATE_REFERENCE:
            enterUnknownState(pctx);
            break;
        case PARSER_STATE_UNKNOWN:
            pctx->unknown_depth++;
            break;
    }
}

static void OnEndElementNs(void *ctx, const char *localname, const char *prefix,
                           const char *URI) {
    TParserCtx *pctx = (TParserCtx *)ctx;
    switch(pctx->state) {
        case PARSER_STATE_INIT:
            break;
        case PARSER_STATE_ALIAS:
            nodeset->aliasArray[nodeset->aliasSize]->id =
                extractNodedId(nodeset->namespaceTable->ns,
                               nodeset->aliasArray[nodeset->aliasSize]->id.idString);
            pctx->state = PARSER_STATE_INIT;
            nodeset->aliasSize++;
            break;
        case PARSER_STATE_URI: {
            int globalIdx = nodeset->namespaceTable->cb(pctx->userContext,
                nodeset->namespaceTable->ns[nodeset->namespaceTable->size - 1].name);

            nodeset->namespaceTable->ns[nodeset->namespaceTable->size - 1].idx =
                (size_t)globalIdx;
            pctx->state = PARSER_STATE_NAMESPACEURIS;
        } break;
        case PARSER_STATE_NAMESPACEURIS:
            pctx->state = PARSER_STATE_INIT;
            break;
        case PARSER_STATE_NODE:
            Nodeset_addNodeToSort(pctx->node);
            pctx->state = PARSER_STATE_INIT;
            if(strEqual(localname, REFERENCETYPE)) {
                Reference *ref = pctx->node->hierachicalRefs;
                while(ref) {
                    if(!ref->isForward) {
                        nodeset->hierachicalRefs[nodeset->hierachicalRefsSize++] =
                            pctx->node->id.idString;
                        break;
                    }
                    ref = ref->next;
                }
                Nodeset_addNodeToSort(pctx->node);
                pctx->state = PARSER_STATE_INIT;
            }
            break;
        case PARSER_STATE_DESCRIPTION:
        case PARSER_STATE_DISPLAYNAME:
        case PARSER_STATE_REFERENCES:
            pctx->state = PARSER_STATE_NODE;
            break;
        case PARSER_STATE_REFERENCE: {
            Reference *ref = pctx->node->hierachicalRefs;
            while(ref) {
                ref->target =
                    extractNodedId(nodeset->namespaceTable->ns, ref->target.idString);
                ref = ref->next;
            }
            ref = pctx->node->nonHierachicalRefs;
            while(ref) {
                ref->target =
                    extractNodedId(nodeset->namespaceTable->ns, ref->target.idString);
                ref = ref->next;
            }
            pctx->state = PARSER_STATE_REFERENCES;
        } break;
        case PARSER_STATE_UNKNOWN:
            pctx->unknown_depth--;
            if(pctx->unknown_depth == 0) {
                pctx->state = pctx->prev_state;
            }
    }
    pctx->nextOnCharacters = NULL;
}

static void OnCharacters(void *ctx, const char *ch, int len) {
    TParserCtx *pctx = (TParserCtx *)ctx;
    if(pctx->state == PARSER_STATE_UNKNOWN || pctx->nextOnCharacters == NULL)
        return;
    char *oldString = pctx->nextOnCharacters[0];
    size_t oldLength = 0;
    if(oldString != NULL) {
        oldLength = strlen(oldString);
    }
    char *newValue = (char *)malloc(oldLength + (size_t)len + 1);
    strncpy(newValue, oldString, oldLength);
    strncpy(newValue + oldLength, ch, (size_t)len);
    nodeset->countedChars[nodeset->charsSize++] = newValue;
    newValue[oldLength + (size_t)len] = '\0';
    pctx->nextOnCharacters[0] = newValue;
}

static xmlSAXHandler make_sax_handler(void) {
    xmlSAXHandler SAXHandler;
    memset(&SAXHandler, 0, sizeof(xmlSAXHandler));
    SAXHandler.initialized = XML_SAX2_MAGIC;
    // nodesets are encoded with UTF-8
    // this code does no transformation on the encoded text or interprets it
    // so it should be safe to cast xmlChar* to char*
    SAXHandler.startElementNs = (startElementNsSAX2Func)OnStartElementNs;
    SAXHandler.endElementNs = (endElementNsSAX2Func)OnEndElementNs;
    SAXHandler.characters = (charactersSAXFunc)OnCharacters;
    return SAXHandler;
}

static int read_xmlfile(FILE *f, TParserCtx *parserCtxt) {
    char chars[1024];
    int res = (int)fread(chars, 1, 4, f);
    if(res <= 0) {
        return 1;
    }

    xmlSAXHandler SAXHander = make_sax_handler();
    xmlParserCtxtPtr ctxt =
        xmlCreatePushParserCtxt(&SAXHander, parserCtxt, chars, res, NULL);
    while((res = (int)fread(chars, 1, sizeof(chars), f)) > 0) {
        if(xmlParseChunk(ctxt, chars, res, 0)) {
            xmlParserError(ctxt, "xmlParseChunk");
            return 1;
        }
    }
    xmlParseChunk(ctxt, chars, 0, 1);
    xmlFreeParserCtxt(ctxt);
    xmlCleanupParser();
    return 0;
}

bool loadFile(const FileHandler *fileHandler) {

    if(fileHandler == NULL) {
        printf("no filehandler - return\n");
        return false;
    }
    if(fileHandler->addNamespace == NULL) {
        printf("no fileHandler->addNamespace - return\n");
        return false;
    }
    if(fileHandler->callback == NULL) {
        printf("no fileHandler->callback - return\n");
        return false;
    }
    bool status = true;
    struct timespec start, startSort, startAdd, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    Nodeset_new(fileHandler->addNamespace);

    TParserCtx *ctx = (TParserCtx *)malloc(sizeof(TParserCtx));
    ctx->state = PARSER_STATE_INIT;
    ctx->prev_state = PARSER_STATE_INIT;
    ctx->unknown_depth = 0;
    ctx->nextOnCharacters = NULL;
    ctx->userContext = fileHandler->userContext;

    FILE *f = fopen(fileHandler->file, "r");
    if(!f) {
        puts("file open error.");
        status = false;
        goto cleanup;
    }

    if(read_xmlfile(f, ctx)) {
        puts("xml read error.");
        status = false;
    }

    clock_gettime(CLOCK_MONOTONIC, &startSort);
    // sorting time missing
    clock_gettime(CLOCK_MONOTONIC, &startAdd);

    if(!Nodeset_getSortedNodes(fileHandler->userContext, fileHandler->callback))
    {
        status = false;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    struct timespec parse = diff(start, startSort);
    struct timespec sort = diff(startSort, startAdd);
    struct timespec add = diff(startAdd, end);
    struct timespec sum = diff(start, end);
    printf("parse (s, ms): %lu %lu\n", parse.tv_sec, parse.tv_nsec / 1000000);
    printf("sort (s, ms): %lu %lu\n", sort.tv_sec, sort.tv_nsec / 1000000);
    printf("add (s, ms): %lu %lu\n", add.tv_sec, add.tv_nsec / 1000000);
    printf("sum (s, ms): %lu %lu\n", sum.tv_sec, sum.tv_nsec / 1000000);

cleanup:
    Nodeset_cleanup();
    free(ctx);
    fclose(f);
    return status;
}